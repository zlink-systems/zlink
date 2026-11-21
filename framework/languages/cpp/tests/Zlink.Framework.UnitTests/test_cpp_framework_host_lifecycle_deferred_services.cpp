/* SPDX-License-Identifier: Elastic-2.0 */

#include "host_lifecycle_tests.hpp"

#include <zlink/json.hpp>

#include <zlink/framework.hpp>
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/location_repository.hpp"
#include "runtime/locations/location_records.hpp"
#include "runtime/spots/spot_runtime.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <functional>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>


namespace zlink::framework::test::host_lifecycle
{
class mesh_started_probe_service_t final : public zlink::framework::hosted_service_t
{
  public:
    mesh_started_probe_service_t (zlink::framework::app_t &app, std::string mesh_name) :
        _app (app), _mesh_name (std::move (mesh_name))
    {
    }

    zlink::framework::task_t<void> start (zlink::framework::service_provider_t &services) override
    {
        (void) services.get_required<zlink::framework::route_mesh_runtime_t> ().snapshot (
          _mesh_name);
        started = true;
        _app.stop ();
        co_return;
    }

    void stop () noexcept override {}

    bool started = false;

  private:
    zlink::framework::app_t &_app;
    std::string _mesh_name;
};

bool verify_deferred_framework_apply_preserves_hosted_service_order ()
{
    constexpr std::string_view mesh_name = "deferred-framework-order-mesh";
    auto app = zlink::framework::app_t::create ();
    auto location_store =
      std::make_shared<zlink::framework::runtime::in_memory_location_store_t> ();
    auto relocation_store =
      std::make_shared<zlink::framework::runtime::in_memory_relocation_store_t> ();
    auto &options = app.add_zlink_framework ();
    options.configure_locations ().owner_lease_renew_interval = std::chrono::milliseconds (10);
    options.configure_locations ().owner_lease_fencing_margin = std::chrono::milliseconds (1);
    options.add_location_store (location_store);
    options.add_relocation_store (relocation_store);
    auto mesh = options.add_route_mesh (std::string (mesh_name));
    mesh.set_object_role (zlink::framework::object_role_t::none)
      .set_routing_id (zlink::routing_id_t::from ("deferred-framework-order-node"))
      .listen ("inproc://deferred-framework-order-node");

    auto probe = std::make_unique<mesh_started_probe_service_t> (app, std::string (mesh_name));
    auto *probe_view = probe.get ();
    app.add_hosted_service (std::move (probe));

    char program[] = "deferred-framework-order";
    char *arguments[] = {program, nullptr};
    try {
        const auto exit_code = app.run (1, arguments);
        if (exit_code == 0 && probe_view->started) {
            probe_view->started = false;
            const bool stopped_run_skips_start =
              app.run (1, arguments) == 0 && !probe_view->started;
            std::cout << "stopped host: rerun-skips-service-start=" << stopped_run_skips_start
                      << '\n';
            return stopped_run_skips_start;
        }
    }
    catch (const std::exception &error) {
        std::cerr << "deferred Framework hosted-service order failed: " << error.what () << '\n';
        return false;
    }
    std::cerr << "deferred Framework hosted service did not start before the application service\n";
    return false;
}
}
