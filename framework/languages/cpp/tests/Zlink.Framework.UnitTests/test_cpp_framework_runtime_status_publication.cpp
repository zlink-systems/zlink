/* SPDX-License-Identifier: Elastic-2.0 */
#include <zlink/framework.hpp>
#include "runtime/locations/in_memory_store_providers.hpp"

#include <atomic>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>

namespace
{
namespace fw = zlink::framework;
class status_service_t final : public fw::hosted_service_t
{
  public:
    explicit status_service_t (std::function<void (fw::service_provider_t &)> check) :
        _check (std::move (check))
    {
    }
    fw::task_t<void> start (fw::service_provider_t &services) override
    {
        _check (services);
        co_return;
    }
    void stop () noexcept override {}

  private:
    std::function<void (fw::service_provider_t &)> _check;
};
class status_event_handler_t
{
  public:
    using event_type = std::string;
    void handle (const event_type &) {}
};
template <class Runtime>
bool same_publication (Runtime &runtime, const std::string &name, fw::topology_state_t expected)
{
    const auto snapshot = runtime.snapshot (name);
    if (snapshot.state != expected || snapshot.is_ready || snapshot.sequence == 0) {
        std::cerr << name << " state=" << static_cast<int> (snapshot.state)
                  << " expected=" << static_cast<int> (expected) << " ready=" << snapshot.is_ready
                  << " sequence=" << snapshot.sequence << '\n';
        return false;
    }
    using status_t = decltype (runtime.snapshot (name));
    std::promise<status_t> first;
    auto result = first.get_future ();
    std::once_flag once;
    auto observation =
      runtime.observe (name, 8, [&] (const fw::observed_status_t<status_t> &value) {
          std::call_once (once, [&] { first.set_value (value.status); });
      });
    const bool delivered = result.wait_for (std::chrono::seconds (5)) == std::future_status::ready;
    const bool same = delivered && result.get ().sequence == snapshot.sequence;
    observation->close ();
    return same && runtime.snapshot (name).sequence == snapshot.sequence;
}
}

int main ()
{
    namespace fw = zlink::framework;
    auto app = fw::app_t::create ();
    bool starting = false;
    app.add_hosted_service (
      std::make_unique<status_service_t> ([&] (fw::service_provider_t &services) {
          starting = same_publication (services.get_required<fw::route_mesh_runtime_t> (), "mesh",
                                       fw::topology_state_t::starting)
                     && same_publication (services.get_required<fw::client_server_runtime_t> (),
                                          "work", fw::topology_state_t::starting)
                     && same_publication (services.get_required<fw::fanout_runtime_t> (), "events",
                                          fw::topology_state_t::starting);
      }));
    app.add_zlink_framework ([] (fw::zlink_framework_options_t &options) {
        options.add_location_store (std::make_shared<fw::runtime::in_memory_location_store_t> ());
        options.add_route_mesh ("mesh")
          .listen ("tcp://127.0.0.1:0")
          .set_routing_id (zlink::routing_id_t::from ("status-before-start"))
          .channel ("registered")
          .server ();
        options.add_client_server_channel ("work").client ();
        options.handlers ().group ("status").add_publish<status_event_handler_t> ();
        options.add_fanout_channel ("events").enable_subscriber ().use_handler_group ("status");
    });
    auto provider = app.advanced ().services ().build_provider ();
    auto &runtime = provider.get_required<fw::framework_runtime_t> ();
    auto &mesh = provider.get_required<fw::route_mesh_runtime_t> ();
    auto &client_server = provider.get_required<fw::client_server_runtime_t> ();
    auto &fanout = provider.get_required<fw::fanout_runtime_t> ();
    if (!same_publication (mesh, "mesh", fw::topology_state_t::starting)
        || !same_publication (client_server, "work", fw::topology_state_t::starting)
        || !same_publication (fanout, "events", fw::topology_state_t::starting)) {
        std::cerr << "registered status before start did not share its publisher\n";
        return 7;
    }
    const auto configured_mesh = mesh.snapshot ("mesh");
    if (configured_mesh.channels.size () != 1
        || configured_mesh.channels.front ().channel_name != "registered"
        || configured_mesh.channels.front ().is_ready)
        return 9;
    const auto initial = runtime.status ();
    if (initial.sequence == 0) {
        std::cerr << "initial status has no sequence\n";
        return 1;
    }
    for (int read = 0; read < 64; ++read) {
        const auto unchanged = runtime.status ();
        if (unchanged.sequence != initial.sequence) {
            std::cerr << "stable host payload unexpectedly changed\n";
            return 2;
        }
        if (unchanged != initial) {
            std::cerr << "unchanged payload replaced published status metadata\n";
            return 3;
        }
    }

    runtime.reset_capacity_metrics ();
    const auto reset = runtime.status ();
    if (reset.capacity.measurement_epoch != initial.capacity.measurement_epoch + 1
        || reset.sequence != initial.sequence + 1) {
        std::cerr << "capacity reset did not publish exactly one new status\n";
        return 4;
    }
    for (int read = 0; read < 64; ++read) {
        if (runtime.status () != reset) {
            std::cerr << "reset status was replaced without a payload change\n";
            return 5;
        }
    }

    std::promise<fw::framework_runtime_status_t> first_observed;
    auto observed_future = first_observed.get_future ();
    std::once_flag first_delivery;
    auto observation = runtime.observe (
      1, [&] (const fw::observed_status_t<fw::framework_runtime_status_t> &observed) {
          std::call_once (first_delivery, [&] { first_observed.set_value (observed.status); });
      });
    const auto observed = observed_future.get ();
    observation->close ();
    if (observed != reset || runtime.status () != observed) {
        std::cerr << "observer and query did not share the published status\n";
        return 6;
    }
    std::atomic<bool> native_starting = false;
    app.add_hosted_service (
      std::make_unique<status_service_t> ([&] (fw::service_provider_t &services) {
          native_starting =
            same_publication (services.get_required<fw::route_mesh_runtime_t> (), "mesh",
                              fw::topology_state_t::starting)
            && same_publication (services.get_required<fw::client_server_runtime_t> (), "work",
                                 fw::topology_state_t::starting)
            && same_publication (services.get_required<fw::fanout_runtime_t> (), "events",
                                 fw::topology_state_t::starting);
      }));
    // Serving is covered by the samples; this harness checks the registered status before the
    // native node exists, while it starts, and after the host stops.
    auto running = std::async (std::launch::async, [&] { return app.run (0, nullptr); });
    const auto started_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
    while (!native_starting && std::chrono::steady_clock::now () < started_deadline)
        std::this_thread::yield ();
    app.stop ();
    const auto exit = running.get ();
    const bool stopped_mesh = same_publication (mesh, "mesh", fw::topology_state_t::stopped);
    const bool stopped_client =
      same_publication (client_server, "work", fw::topology_state_t::stopped);
    const bool stopped_fanout = same_publication (fanout, "events", fw::topology_state_t::stopped);
    if (exit != 0 || !starting || !native_starting || !stopped_mesh || !stopped_client
        || !stopped_fanout) {
        std::cerr << "topology lifecycle publication: exit=" << exit << " starting=" << starting
                  << " native_starting=" << native_starting << " stopped=" << stopped_mesh << ','
                  << stopped_client << ',' << stopped_fanout << '\n';
        return 8;
    }
    std::cout << "registered topology status before start, while starting and after stop passed\n";
}
