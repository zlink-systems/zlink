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

#include "../support/host_lifecycle_actor_fixture.hpp"
#include "../support/host_lifecycle_relocation_ready_fixture.hpp"

namespace zlink::framework::test::host_lifecycle
{
class configuration_instance_spot_t final : public zlink::framework::instance_spot_t
{
  public:
    explicit configuration_instance_spot_t (zlink::framework::instance_spot_context_t context) :
        _context (std::move (context))
    {
    }

    zlink::framework::instance_spot_context_t &context () noexcept override { return _context; }
    const zlink::framework::instance_spot_context_t &context () const noexcept override
    {
        return _context;
    }
    void configure () override {}

  private:
    zlink::framework::instance_spot_context_t _context;
};

bool expect_not_configured (
  std::string_view label,
  std::function<void (zlink::framework::zlink_framework_options_t &)> configure)
{
    auto app = zlink::framework::app_t::create ();
    try {
        app.add_zlink_framework (std::move (configure));
    }
    catch (const zlink::framework::framework_exception_t &error) {
        if (error.kind () == zlink::framework::framework_error_kind_t::not_configured)
            return true;
        std::cerr << label << " must fail with not_configured\n";
        return false;
    }
    std::cerr << label << " must fail before listener startup\n";
    return false;
}

bool verify_object_store_configuration_preflight ()
{
    auto unselected = zlink::framework::app_t::create ();
    try {
        unselected.add_zlink_framework ([] (zlink::framework::zlink_framework_options_t &options) {
            options.add_route_mesh ("unselected-object-role")
              .listen ("inproc://unselected-object-role");
        });
    }
    catch (const std::exception &error) {
        std::cerr << "unselected Object role must not require a Location Store: " << error.what ()
                  << '\n';
        return false;
    }

    if (!expect_not_configured ("Object Client without Location Store",
                                [] (zlink::framework::zlink_framework_options_t &options) {
                                    options.add_route_mesh ("missing-client-location")
                                      .set_object_role (zlink::framework::object_role_t::client)
                                      .listen ("inproc://missing-client-location");
                                }))
        return false;
    if (!expect_not_configured ("Object Server without Location Store",
                                [] (zlink::framework::zlink_framework_options_t &options) {
                                    options.add_route_mesh ("missing-server-location")
                                      .set_object_role (zlink::framework::object_role_t::server)
                                      .listen ("inproc://missing-server-location");
                                }))
        return false;

    auto location_store =
      std::make_shared<zlink::framework::runtime::in_memory_location_store_t> ();
    if (!expect_not_configured (
          "relocatable Actor without Relocation Store",
          [location_store] (zlink::framework::zlink_framework_options_t &options) {
              options.add_location_store (location_store);
              options.add_route_mesh ("missing-actor-relocation")
                .set_object_role (zlink::framework::object_role_t::server)
                .listen ("inproc://missing-actor-relocation")
                .add_actor_factory<configuration_actor_t, configuration_actor_factory_t> (
                  "configuration-actor", std::make_shared<configuration_actor_factory_t> (),
                  [] (auto &factory) { factory.recreate_on_relocation (); });
          }))
        return false;
    if (!expect_not_configured (
          "relocatable User Spot without Relocation Store",
          [location_store] (zlink::framework::zlink_framework_options_t &options) {
              options.add_location_store (location_store);
              options.add_route_mesh ("missing-spot-relocation")
                .set_object_role (zlink::framework::object_role_t::server)
                .listen ("inproc://missing-spot-relocation")
                .add_spot_factory<relocation_ready_spot_t> (
                  "configuration-spot",
                  [] (zlink::framework::spot_context_t context) {
                      return std::make_shared<relocation_ready_spot_t> (std::move (context));
                  },
                  [] (auto &factory) { factory.recreate_on_relocation (); });
          }))
        return false;
    if (!expect_not_configured (
          "Instance Spot without Relocation Store",
          [location_store] (zlink::framework::zlink_framework_options_t &options) {
              options.add_location_store (location_store);
              options.add_route_mesh ("missing-instance-relocation")
                .set_object_role (zlink::framework::object_role_t::server)
                .listen ("inproc://missing-instance-relocation")
                .add_instance_spot_factory<configuration_instance_spot_t> (
                  "configuration-instance",
                  [] (zlink::framework::instance_spot_context_t context) {
                      return std::make_shared<configuration_instance_spot_t> (std::move (context));
                  },
                  [] (auto &factory) { factory.disable_relocation (); });
          }))
        return false;

    auto disabled = zlink::framework::app_t::create ();
    try {
        disabled.add_zlink_framework (
          [location_store] (zlink::framework::zlink_framework_options_t &options) {
              options.add_location_store (location_store);
              auto mesh = options.add_route_mesh ("disabled-relocation");
              mesh.set_object_role (zlink::framework::object_role_t::server);
              mesh.listen ("inproc://disabled-relocation");
              mesh.add_actor_factory<configuration_actor_t, configuration_actor_factory_t> (
                "disabled-actor", std::make_shared<configuration_actor_factory_t> (),
                [] (auto &factory) { factory.disable_relocation (); });
              mesh.add_spot_factory<relocation_ready_spot_t> (
                "disabled-spot",
                [] (zlink::framework::spot_context_t context) {
                    return std::make_shared<relocation_ready_spot_t> (std::move (context));
                },
                [] (auto &factory) { factory.disable_relocation (); });
          });
    }
    catch (const std::exception &error) {
        std::cerr << "disabled factories must not require a Relocation Store: " << error.what ()
                  << '\n';
        return false;
    }
    return true;
}
}
