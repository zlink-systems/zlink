/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "metric_test_reader.hpp"
#include "metric_test_store.hpp"
#include <zlink/framework.hpp>
#include <atomic>
#include <iostream>
#include <future>
#include <optional>
#include <thread>

using namespace zlink::framework;
using namespace std::chrono_literals;
namespace
{
void require (bool value, const char *message)
{
    if (!value)
        throw std::runtime_error (message);
}
std::optional<double> point (metric_test::provider_t &provider,
                             const std::string &name,
                             const std::string &label = "",
                             const std::string &value = "")
{
    for (const auto &metric : provider.collect ()) {
        if (metric.instrument_descriptor.name_ != name)
            continue;
        for (const auto &point : metric.point_data_attr_) {
            if (!label.empty ()) {
                const auto found = point.attributes.find (label);
                if (found == point.attributes.end ()
                    || opentelemetry::nostd::get<std::string> (found->second) != value)
                    continue;
            }
            if (const auto *sum =
                  opentelemetry::nostd::get_if<metric_test::sdk::SumPointData> (&point.point_data))
                return opentelemetry::nostd::get<double> (sum->value_);
            if (const auto *gauge =
                  opentelemetry::nostd::get_if<metric_test::sdk::LastValuePointData> (
                    &point.point_data))
                return opentelemetry::nostd::get<double> (gauge->value_);
            if (const auto *histogram =
                  opentelemetry::nostd::get_if<metric_test::sdk::HistogramPointData> (
                    &point.point_data))
                return static_cast<double> (histogram->count_);
        }
    }
    return std::nullopt;
}
struct actor final : actor_t
{
    explicit actor (actor_context_t context) : _context (std::move (context)) {}
    actor_context_t &context () noexcept override { return _context; }
    const actor_context_t &context () const noexcept override { return _context; }
    actor_context_t _context;
};
struct spot final : spot_t<actor>
{
    explicit spot (spot_context_t context) : _context (std::move (context)) {}
    spot_context_t &context () noexcept override { return _context; }
    const spot_context_t &context () const noexcept override { return _context; }
    void configure () override {}
    task_t<spot_actor_join_result_t> on_actor_join (std::string_view, const message_t &) override
    {
        co_return spot_actor_join_result_t::reject ();
    }
    task_t<void> on_actor_joined (actor &) override { co_return; }
    task_t<void> on_leave_actor (actor &) override { co_return; }
    spot_context_t _context;
};
struct access final : hosted_service_t
{
    spot_manager_t *spots = nullptr;
    task_t<void> start (service_provider_t &services) override
    {
        spots = &services.get_required<spot_manager_t> ();
        co_return;
    }
    void stop () noexcept override {}
};
struct running
{
    app_t &app;
    std::atomic_int exit_code{-1};
    std::thread worker;
    explicit running (app_t &value) :
        app (value), worker ([this] {
            char program[] = "metric-provider-contract";
            char *argv[] = {program, nullptr};
            exit_code = app.run (1, argv);
        })
    {
    }
    ~running () { stop (); }
    void stop ()
    {
        app.stop ();
        if (worker.joinable ())
            worker.join ();
    }
    void ready ()
    {
        const auto until = std::chrono::steady_clock::now () + 2s;
        while (!app.is_ready () && exit_code == -1 && std::chrono::steady_clock::now () < until)
            std::this_thread::yield ();
        require (app.is_ready (), "host did not become ready");
    }
};
void contract ()
{
    metric_test::provider_t global;
    metric_test::provider_t first (false), second (false), rejected (false);
    auto app = app_t::create ();
    app.add_zlink_framework ([] (auto &options) {
        options.add_location_store (std::make_shared<metric_test::store_t> ());
        options.add_route_mesh ("provider-mesh")
          .listen ("tcp://127.0.0.1:0")
          .set_routing_id (zlink::routing_id_t::from ("provider-node"))
          .objects ()
          .server ()
          .template add_spot_factory<spot> ("provider-spot")
          .disable_relocation ();
    });
    // Configure after the host and Framework registration already exist.
    app.use_metric_provider (first.api ());
    require (point (first, "zlink.host.state", "state", "preparing") == 1,
             "Preparing state missing");
    (void) app.relocate ({}).result ().value ();
    require (point (first, "zlink.host.relocation.blocked") == 1, "event counter missing");
    auto service = std::make_unique<access> ();
    auto *services = service.get ();
    app.add_hosted_service (std::move (service));
    running host (app);
    host.ready ();
    require (point (first, "zlink.host.state", "state", "serving") == 1, "Serving state missing");
    auto other = app_t::create ();
    other.use_metric_provider (second.api ());
    require (point (second, "zlink.host.state", "state", "preparing") == 1, "second host missing");
    require (!point (second, "zlink.host.relocation.blocked"), "hosts shared a provider");
    auto created = services->spots->create ("provider-spot")
                     .in_mesh ("provider-mesh")
                     .async ()
                     .result ()
                     .value ();
    require (created.state == spot_create_state_t::created, "Spot creation failed");
    require (point (first, "zlink.spot.count", "spot_kind", "user") == 1,
             "Spot creation sample missing");
    require (services->spots->close (created.spot).result ().value (), "Spot close failed");
    require (point (first, "zlink.spot.count", "spot_kind", "user") == 0,
             "Spot close sample missing");
    bool refused = false;
    try {
        app.use_metric_provider (rejected.api ());
    }
    catch (const std::logic_error &) {
        refused = true;
    }
    require (refused, "provider replacement after run was accepted");
    require (rejected.collect ().empty (), "rejected provider received records");
    require (global.collect ().empty (), "Framework recorded to global provider");
    host.stop ();
    require (host.exit_code == 0, "host failed");
    require (point (first, "zlink.host.shutdown.duration") == 1, "event histogram missing");
    require (point (first, "zlink.host.state", "state", "stopped") == 1, "Stopped state missing");
    // SDK collection and callback removal must synchronize while the owner is destroyed.
    metric_test::provider_t collecting (false);
    auto transient = std::make_unique<app_t> ();
    transient->use_metric_provider (collecting.api ());
    std::atomic_bool finish{false};
    std::promise<void> collected;
    auto first_collection = collected.get_future ();
    std::thread collector ([&] {
        (void) collecting.collect ();
        collected.set_value ();
        while (!finish.load (std::memory_order_acquire))
            (void) collecting.collect ();
    });
    first_collection.get ();
    transient.reset ();
    finish.store (true, std::memory_order_release);
    collector.join ();
    auto noop = app_t::create ();
    running no_metrics (noop);
    no_metrics.ready ();
    no_metrics.stop ();
    auto empty = app_t::create ();
    empty.use_metric_provider ({});
    (void) empty.shutdown (1s).result ().value ();
    require (global.collect ().empty (), "unspecified provider used global SDK");
    require (second.collect ().size () == 1, "other host received unrelated instruments");
}
} // namespace
int main ()
{
    try {
        contract ();
        std::cout << "metric provider contract passed\n";
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << '\n';
        return 1;
    }
}
