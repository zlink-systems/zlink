/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/host_lifecycle_ready_probe.hpp"
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
bool verify_mesh_discovery_stop_wakes_polling_waits ()
{
    bool passed = false;
    {
        auto app = zlink::framework::app_t::create ();
        auto &options = app.add_zlink_framework ();
        options.configure_locations ().owner_lease_renew_interval = std::chrono::milliseconds (10);
        options.configure_locations ().owner_lease_fencing_margin = std::chrono::milliseconds (1);
        options.add_location_store (
          std::make_shared<zlink::framework::runtime::in_memory_location_store_t> ());
        options.configure_locations ().polling_interval = std::chrono::hours (24);
        for (const auto *name : {"stop-discovery-a", "stop-discovery-b"}) {
            auto mesh = options.add_route_mesh (name);
            mesh.channel_name (std::string (name) + "-channel").server ();
            mesh.set_object_role (zlink::framework::object_role_t::server)
              .set_routing_id (zlink::routing_id_t::from (name))
              .listen ("tcp://127.0.0.1:0");
        }
        auto probe = std::make_unique<ready_host_probe_t> ();
        auto *ready = probe.get ();
        app.add_hosted_service (std::move (probe));
        char program[] = "stop-mesh-discovery";
        char *arguments[] = {program, nullptr};
        int exit_code = -1;
        std::thread run_thread ([&] { exit_code = app.run (1, arguments); });
        const bool serving = ready->wait_ready ();
        const auto first = app.shutdown (std::chrono::seconds (2)).result ().value ();
        const auto repeated = app.shutdown (std::chrono::seconds (2)).result ().value ();
        run_thread.join ();
        passed = serving && first.outcome == zlink::framework::termination_outcome_t::stopped
                 && repeated.outcome == zlink::framework::termination_outcome_t::stopped
                 && first.reason == zlink::framework::termination_reason_t::none
                 && repeated.reason == zlink::framework::termination_reason_t::none
                 && exit_code == 0;
    }
    std::cout << "two-mesh stop: repeated-shutdown-and-destruction=" << passed << '\n';
    return passed;
}
}
