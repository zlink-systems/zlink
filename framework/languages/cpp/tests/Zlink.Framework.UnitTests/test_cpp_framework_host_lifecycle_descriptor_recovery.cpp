/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/host_lifecycle_degraded_object_spot.hpp"
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
#include "../support/host_lifecycle_location_recovery_fixture.hpp"

#include "../support/host_lifecycle_wait.hpp"

namespace zlink::framework::test::host_lifecycle
{
struct degraded_channel_message_t
{
};
}

template <>
struct zlink::json::dto_traits<zlink::framework::test::host_lifecycle::degraded_channel_message_t>
{
    static constexpr auto fields = std::tuple{};
};

namespace zlink::framework::test::host_lifecycle
{
struct degraded_channel_handler_t
{
    using message_type = degraded_channel_message_t;

    void handle (const degraded_channel_message_t &) {}
};

class degraded_location_host_probe_t final : public zlink::framework::hosted_service_t
{
  public:
    degraded_location_host_probe_t (zlink::framework::app_t &app,
                                    std::shared_ptr<recovering_location_store_t> store) :
        _app (app), _store (std::move (store))
    {
    }

    zlink::framework::task_t<void> start (zlink::framework::service_provider_t &services) override
    {
        auto *location_repository =
          &services.get_required<zlink::framework::location_repository_t> ();
        auto manager = services.get_required<zlink::framework::spot_manager_t> ();
        auto *serializers = &services.get_required<zlink::framework::serializer_registry_t> ();
        _worker = std::thread ([this, location_repository, manager = std::move (manager),
                                serializers] () mutable {
            auto &locations = *location_repository;
            auto client = _app.advanced ().zlink ().route_client (*serializers);
            preparing_without_lease =
              !_app.is_ready ()
              && _app.runtime_state () == zlink::framework::framework_runtime_state_t::preparing;
            descriptor_blocked =
              locations.list_mesh_nodes ("degraded-location-host").result ().value ().items.empty ()
              && locations.list_client_servers ("degraded-client-server")
                   .result ()
                   .value ()
                   .items.empty ()
              && locations.list_fanout_publishers ("degraded-fanout")
                   .result ()
                   .value ()
                   .items.empty ();

            const auto blocked_create =
              manager
                .get_or_create (zlink::framework::spot_id_t ("degraded-object"), "degraded-object")
                .in_mesh ("degraded-location-host")
                .timeout (std::chrono::milliseconds (50))
                .async ()
                .result ();
            factory_blocked =
              !blocked_create
              && degraded_object_spot_t::factory_count.load (std::memory_order_acquire) == 0;
            const auto blocked_message =
              client
                .send_to_spot (zlink::framework::spot_id_t ("degraded-object"),
                               degraded_object_message_t{17})
                .async ()
                .result ();
            message_and_timer_blocked =
              !blocked_message
              && degraded_object_spot_t::message_value.load (std::memory_order_acquire) == 0
              && degraded_object_timer_handler_t::timer_count.load (std::memory_order_acquire) == 0;
            _store->recover ();
            const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (1);
            while (std::chrono::steady_clock::now () < deadline) {
                const auto mesh =
                  locations.list_mesh_nodes ("degraded-location-host").result ().value ();
                const auto client_server =
                  locations.list_client_servers ("degraded-client-server").result ().value ();
                const auto fanout =
                  locations.list_fanout_publishers ("degraded-fanout").result ().value ();
                mesh_republished = !mesh.items.empty () && !mesh.items.front ().owner_id.empty ()
                                   && mesh.items.front ().lease_generation > 0;
                client_server_republished = !client_server.items.empty ()
                                            && !client_server.items.front ().owner_id.empty ()
                                            && client_server.items.front ().lease_generation > 0;
                fanout_republished = !fanout.items.empty ()
                                     && !fanout.items.front ().owner_id.empty ()
                                     && fanout.items.front ().lease_generation > 0;
                if (mesh_republished && client_server_republished && fanout_republished) {
                    descriptor_republished = true;
                    break;
                }
                std::this_thread::sleep_for (std::chrono::milliseconds (1));
            }
            const auto resumed_create =
              manager
                .get_or_create (zlink::framework::spot_id_t ("degraded-object"), "degraded-object")
                .in_mesh ("degraded-location-host")
                .timeout (std::chrono::seconds (1))
                .async ()
                .result ();
            factory_resumed =
              resumed_create
              && degraded_object_spot_t::factory_count.load (std::memory_order_acquire) == 1;
            const auto resumed_message =
              client
                .send_to_spot (zlink::framework::spot_id_t ("degraded-object"),
                               degraded_object_message_t{17})
                .async ()
                .result ();
            message_resumed = static_cast<bool> (resumed_message);
            if (!resumed_message)
                resumed_message_error = resumed_message.error () ? resumed_message.error ()->what ()
                                                                 : "message submission failed";
            message_and_timer_resumed =
              resumed_message
              && wait_until (
                [] {
                    return degraded_object_spot_t::message_value.load (std::memory_order_acquire)
                             == 17
                           && degraded_object_timer_handler_t::timer_count.load (
                                std::memory_order_acquire)
                                > 0;
                },
                std::chrono::seconds (1));
            _app.stop ();
        });
        co_return;
    }

    void stop () noexcept override
    {
        if (_worker.joinable ())
            _worker.join ();
    }

    bool preparing_without_lease = false;
    bool descriptor_blocked = false;
    bool descriptor_republished = false;
    bool mesh_republished = false;
    bool client_server_republished = false;
    bool fanout_republished = false;
    bool factory_blocked = false;
    bool factory_resumed = false;
    bool message_and_timer_blocked = false;
    bool message_and_timer_resumed = false;
    bool message_resumed = false;
    std::string resumed_message_error;

  private:
    zlink::framework::app_t &_app;
    std::shared_ptr<recovering_location_store_t> _store;
    std::thread _worker;
};

bool verify_degraded_host_republishes_descriptor_after_owner_claim ()
{
    degraded_object_spot_t::factory_count.store (0, std::memory_order_release);
    degraded_object_spot_t::message_value.store (0, std::memory_order_release);
    degraded_object_timer_handler_t::timer_count.store (0, std::memory_order_release);
    auto app = zlink::framework::app_t::create ();
    auto store = std::make_shared<recovering_location_store_t> ();
    auto relocation_store =
      std::make_shared<zlink::framework::runtime::in_memory_relocation_store_t> ();
    constexpr auto renew_timeout = std::chrono::milliseconds (500);
    auto &options = app.add_zlink_framework ();
    options.add_location_store (store);
    options.add_relocation_store (relocation_store);
    auto &locations = options.configure_locations ();
    locations.owner_lease_renew_interval = std::chrono::milliseconds (10);
    locations.owner_lease_renew_timeout = renew_timeout;
    locations.owner_lease_ttl = std::chrono::seconds (3);
    locations.owner_lease_fencing_margin = std::chrono::milliseconds (1);
    locations.polling_interval = std::chrono::milliseconds (5);
    auto degraded_mesh = options.add_route_mesh ("degraded-location-host");
    degraded_mesh.channel_name ("degraded-object-channel").server ();
    degraded_mesh.set_object_role (zlink::framework::object_role_t::server)
      .set_routing_id (zlink::routing_id_t::from ("degraded-location-host-node"))
      .listen ("inproc://degraded-location-host-node")
      .add_spot_factory<degraded_object_spot_t> (
        "degraded-object",
        [] (zlink::framework::spot_context_t context) {
            degraded_object_spot_t::factory_count.fetch_add (1, std::memory_order_acq_rel);
            return std::make_shared<degraded_object_spot_t> (std::move (context));
        },
        [] (auto &factory) { factory.recreate_on_relocation (); });
    options.add_client_server_channel ("degraded-client-server")
      .server ()
      .listen (0)
      .add_send_handler<degraded_channel_handler_t, degraded_channel_message_t> ();
    options.add_fanout_channel ("degraded-fanout")
      .enable_publisher (0)
      .set_routing_id (zlink::routing_id_t::from ("degraded-fanout-publisher"));

    auto probe = std::make_unique<degraded_location_host_probe_t> (app, store);
    auto *probe_view = probe.get ();
    app.add_hosted_service (std::move (probe));
    char program[] = "degraded-location-host";
    char *arguments[] = {program, nullptr};
    const auto exit_code = app.run (1, arguments);
    if (exit_code == 0 && probe_view->preparing_without_lease && probe_view->descriptor_blocked
        && probe_view->descriptor_republished && probe_view->factory_blocked
        && probe_view->factory_resumed && probe_view->message_and_timer_blocked
        && probe_view->message_and_timer_resumed)
        return true;
    std::cerr << "degraded Location host did not gate and republish its descriptor"
              << " preparing-without-lease=" << probe_view->preparing_without_lease
              << " blocked=" << probe_view->descriptor_blocked
              << " republished=" << probe_view->descriptor_republished
              << " mesh=" << probe_view->mesh_republished
              << " client-server=" << probe_view->client_server_republished
              << " fanout=" << probe_view->fanout_republished
              << " factory-blocked=" << probe_view->factory_blocked
              << " factory-resumed=" << probe_view->factory_resumed
              << " work-blocked=" << probe_view->message_and_timer_blocked
              << " work-resumed=" << probe_view->message_and_timer_resumed
              << " send-resumed=" << probe_view->message_resumed
              << " message-value=" << degraded_object_spot_t::message_value.load ()
              << " timer-count=" << degraded_object_timer_handler_t::timer_count.load ()
              << " message-error=" << probe_view->resumed_message_error << '\n';
    return false;
}
}
