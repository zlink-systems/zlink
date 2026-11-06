/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <zlink/framework.hpp>
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/location_runtime.hpp"

#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string_view>
#include <thread>

using namespace zlink::framework;

namespace
{
class ready_probe_t final : public hosted_service_t
{
  public:
    task_t<void> start (service_provider_t &services) override
    {
        _observer =
          services.get_required<framework_runtime_t> ().observe (1, [this] (const auto &event) {
              {
                  std::lock_guard lock (_mutex);
                  _state = event.status.state;
              }
              _changed.notify_all ();
          });
        co_return;
    }
    void stop () noexcept override
    {
        if (_observer)
            _observer->close ();
    }
    bool ready ()
    {
        std::unique_lock lock (_mutex);
        _changed.wait (lock, [this] {
            return _state == framework_runtime_state_t::serving
                   || _state == framework_runtime_state_t::stopped
                   || _state == framework_runtime_state_t::error;
        });
        return _state == framework_runtime_state_t::serving;
    }

  private:
    std::mutex _mutex;
    std::condition_variable _changed;
    framework_runtime_state_t _state = framework_runtime_state_t::preparing;
    std::unique_ptr<runtime_observation_t> _observer;
};

bool verify_none (bool registered)
{
    auto app = app_t::create ();
    app.add_zlink_framework ([registered] (zlink_framework_options_t &options) {
        if (registered) {
            options.configure_locations ().owner_lease_renew_interval =
              std::chrono::milliseconds (10);
            options.configure_locations ().owner_lease_fencing_margin =
              std::chrono::milliseconds (1);
            options.add_location_store (std::make_shared<runtime::in_memory_location_store_t> ());
        }
        auto mesh = options.add_route_mesh ("optional-store")
                      .set_object_role (object_role_t::none)
                      .set_routing_id (zlink::routing_id_t::from ("optional-store-node"))
                      .listen ("tcp://127.0.0.1:*");
        mesh.channel_name ("optional-weight").server ();
    });
    auto services = app.advanced ().services ().build_provider ();
    auto repository = services.get<location_repository_t> ();
    auto locations = services.get<runtime::location_runtime_t> ();
    if (bool (repository) != registered || bool (locations) != registered) {
        std::cerr << "Store registration must determine repository and runtime presence\n";
        return false;
    }
    auto probe = std::make_unique<ready_probe_t> ();
    auto *ready = probe.get ();
    probe->start (services).result ().value ();
    char name[] = "optional-store";
    char *args[] = {name, nullptr};
    int exit_code = -1;
    std::thread runner ([&] { exit_code = app.run (1, args); });
    bool passed = ready->ready ();
    try {
        if (passed) {
            auto &weights =
              services.get_required<route_mesh_runtime_options_t> ().mesh ("optional-store");
            weights.placement_weight (321);
            auto &channel =
              services.get_required<route_mesh_runtime_options_t> ().channel ("optional-weight");
            channel.weight (654);
            passed = weights.placement_weight () == 321 && channel.weight () == 654;
            if (registered) {
                const auto rows =
                  repository->get ().list_mesh_nodes ("optional-store").result ().value ();
                passed = passed && locations->get ().current_owner_token ().has_value ()
                         && rows.items.size () == 1
                         && rows.items.front ().object_role == object_role_t::none
                         && rows.items.front ().placement_weight == 321
                         && rows.items.front ().channel_weights.at ("optional-weight") == 654;
            }
        }
    }
    catch (const std::exception &error) {
        std::cerr << "None runtime weight update: " << error.what () << '\n';
        passed = false;
    }
    app.request_stop ();
    runner.join ();
    probe->stop ();
    services.close ();
    std::cout << "none registered=" << registered << " passed=" << passed << " exit=" << exit_code
              << '\n';
    return passed && exit_code == 0;
}

bool verify_missing (object_role_t role)
{
    auto app = app_t::create ();
    try {
        app.add_zlink_framework ([role] (zlink_framework_options_t &options) {
            options.add_route_mesh ("missing-store")
              .set_object_role (role)
              .listen ("tcp://127.0.0.1:*");
        });
    }
    catch (const framework_exception_t &error) {
        return error.kind () == framework_error_kind_t::not_configured
               && !app.advanced ().services ().build_provider ().get<location_repository_t> ();
    }
    return false;
}
}

int main (int argc, char **argv)
{
    completion_owner_t completion_owner;
    const std::string_view selected = argc > 1 ? argv[1] : "all";
    bool passed = true;
    if (selected == "all" || selected == "none")
        passed = verify_none (false) && passed;
    if (selected == "all" || selected == "store")
        passed = verify_none (true) && passed;
    if (selected == "all" || selected == "client")
        passed = verify_missing (object_role_t::client) && passed;
    if (selected == "all" || selected == "server")
        passed = verify_missing (object_role_t::server) && passed;
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
