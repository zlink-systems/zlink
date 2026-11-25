/* SPDX-License-Identifier: Elastic-2.0 */
#include <zlink/json.hpp>
#include "../support/runtime_failure_fixture.hpp"
#include "../support/host_readiness.hpp"
#include "../support/location_reclaim_clock.hpp"

#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/locations/location_auto_connect_host_service.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/channels/channel_runtime_manager.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/client_server/client_server_location_runtime.hpp"
#include "runtime/mesh/user_spot_terminal_mapping.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include "runtime/streams/stream_host_service.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "../support/owner_lease_time_store.hpp"

#include <gtest/gtest.h>
#include <zlink/framework.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "../support/store_location_resolver_user_spot_fixture.hpp"

namespace
{
using namespace zlink::framework::test::store_location_resolvers;

using zlink::framework::actor_location_t;
using zlink::framework::location_kind_t;
using zlink::framework::location_object_entry_t;
using zlink::framework::location_object_filter_t;
using zlink::framework::location_options_t;
using zlink::framework::location_owner_token_t;
using zlink::framework::location_page_request_t;
using zlink::framework::location_page_t;
using zlink::framework::location_role_t;
using zlink::framework::location_runtime_query_t;
using zlink::framework::location_runtime_status_t;
using zlink::framework::location_service_summary_filter_t;
using zlink::framework::location_service_summary_t;
using zlink::framework::location_topology_entry_t;
using zlink::framework::location_topology_filter_t;
using zlink::framework::location_topology_state_t;
using zlink::framework::location_write_intent_t;
using zlink::framework::location_write_status_t;
using zlink::framework::route_kind_t;
using zlink::framework::route_location_key_t;
using zlink::framework::spot_location_t;
using zlink::framework::runtime::in_memory_location_repository_t;
using zlink::framework::runtime::in_memory_location_store_t;
using zlink::framework::runtime::location_auto_connect_host_service_t;
using zlink::framework::runtime::location_runtime_t;
using zlink::framework::runtime::store_location_resolvers_t;
using zlink::framework::runtime::store_location_runtime_query_t;

class lease_target_instance_spot_t final : public zlink::framework::instance_spot_t
{
  public:
    explicit lease_target_instance_spot_t (zlink::framework::instance_spot_context_t context) :
        _context (std::move (context))
    {
    }

    zlink::framework::instance_spot_context_t &context () noexcept override { return _context; }

    const zlink::framework::instance_spot_context_t &context () const noexcept override
    {
        return _context;
    }

    void configure () override
    {
        _context.handlers ().add_handler<&lease_target_instance_spot_t::handle> ();
    }

    lease_target_reply_t handle (const lease_target_request_t &)
    {
        return {std::string (_context.node_rid ().value ())};
    }

  private:
    zlink::framework::instance_spot_context_t _context;
};

class descriptor_owner_lease_client_t final : public zlink::framework::hosted_service_t
{
  public:
    descriptor_owner_lease_client_t (
      zlink::framework::app_t &app,
      std::string user_spot_id,
      std::string instance_spot_id,
      std::shared_ptr<zlink::framework::tests::owner_lease_time_store_t> observed_store = {}) :
        _app (&app),
        _user_spot_id (std::move (user_spot_id)),
        _instance_spot_id (std::move (instance_spot_id)),
        _observed_store (std::move (observed_store))
    {
    }

    zlink::framework::task_t<void> start (zlink::framework::service_provider_t &services) override
    {
        _worker = std::jthread ([this, services] () mutable {
            zlink::framework::test::wait_for_serving (services);
            execute (services).result ().value ();
        });
        co_return;
    }

    zlink::framework::task_t<void> execute (zlink::framework::service_provider_t &services)
    {
        if (_observed_store) {
            auto &discovery =
              services.get_required<zlink::framework::runtime::live_location_reader_t> ();
            const auto observed = discovery.poll_mesh_sources ("lease-target-mesh").result ();
            if (observed) {
                expired_descriptor_observed = std::any_of (
                  observed.value ()->begin (), observed.value ()->end (), [] (const auto &row) {
                      return row.descriptor.rid.to_string () == "aaa-stale-target"
                             && !row.available ();
                  });
            } else {
                discovery_error = observed.error_kind ();
            }
        }
        try {
            auto user = services.get_required<zlink::framework::spot_manager_t> ()
                          .get_or_create (zlink::framework::spot_id_t (_user_spot_id), "room")
                          .in_mesh ("lease-target-mesh")
                          .timeout (std::chrono::seconds (2))
                          .async ()
                          .result ();
            if (user)
                user_target = std::string (user.value ().spot.node_rid ().value ());
            else
                user_error = user.error_kind ();
        }
        catch (const zlink::framework::framework_exception_t &error) {
            user_error = error.kind ();
        }
        catch (const std::exception &error) {
            user_exception = error.what ();
        }
        auto instance_flow = observe_instance_request_flow ();
        try {
            auto route_client = _app->advanced ().zlink ().route_client (
              services.get_required<zlink::framework::serializer_registry_t> ());
            auto reply = route_client.request_to_spot (_instance_spot_id, lease_target_request_t{7})
                           .instance_spot ("cart")
                           .in_mesh ("lease-target-mesh")
                           .timeout (std::chrono::seconds (2))
                           .async<lease_target_reply_t> ()
                           .result ();
            if (reply)
                instance_target = reply.value ().node_rid;
            else
                instance_error = reply.error_kind ();
        }
        catch (const zlink::framework::framework_exception_t &error) {
            instance_error = error.kind ();
        }
        catch (const std::exception &error) {
            instance_exception = error.what ();
        }
        if (_observed_store)
            instance_owner_checked_before_authority =
              _observed_store->owner_was_read_in_expected_order ();
        instance_flow.reset ();

        _app->stop ();
        co_return;
    }

    void stop () noexcept override {}

    std::optional<zlink::framework::runtime::flow_context_t::scope_t>
    observe_instance_request_flow ()
    {
        if (!_observed_store)
            return std::nullopt;
        auto flow_id = zlink::framework::runtime::flow_id_t::create ();
        _observed_store->observe_owner_read_order (_instance_spot_id, 1, flow_id);
        return std::optional<zlink::framework::runtime::flow_context_t::scope_t> (
          std::in_place, zlink::framework::runtime::flow_value_t{
                           std::move (flow_id), zlink::framework::flow_origin_t::application,
                           zlink::framework::message_flow_log_mode_t::normal, std::nullopt});
    }

    std::optional<std::string> user_target;
    std::optional<zlink::framework::framework_error_kind_t> user_error;
    std::string user_exception;
    bool expired_descriptor_observed = false;
    std::optional<zlink::framework::framework_error_kind_t> discovery_error;
    std::optional<std::string> instance_target;
    std::optional<zlink::framework::framework_error_kind_t> instance_error;
    std::string instance_exception;
    std::optional<bool> instance_owner_checked_before_authority;

  private:
    zlink::framework::app_t *_app;
    std::string _user_spot_id;
    std::string _instance_spot_id;
    std::shared_ptr<zlink::framework::tests::owner_lease_time_store_t> _observed_store;

  private:
    std::jthread _worker;
};

struct descriptor_owner_lease_result_t
{
    int app_result = -1;
    std::optional<std::string> user_target;
    std::optional<zlink::framework::framework_error_kind_t> user_error;
    std::string user_exception;
    bool expired_descriptor_observed = false;
    std::optional<zlink::framework::framework_error_kind_t> discovery_error;
    std::optional<std::string> instance_target;
    std::optional<zlink::framework::framework_error_kind_t> instance_error;
    std::string instance_exception;
    std::optional<bool> instance_owner_checked_before_authority;
};

std::string target_id_selected_from_range (std::string prefix,
                                           std::uint64_t modulus,
                                           std::uint64_t lower_bound,
                                           std::uint64_t upper_bound)
{
    for (std::uint64_t suffix = 0;; ++suffix) {
        auto candidate = prefix + std::to_string (suffix);
        const auto choice = std::hash<std::string>{}(candidate) % modulus;
        if (choice >= lower_bound && choice < upper_bound)
            return candidate;
    }
}

descriptor_owner_lease_result_t run_descriptor_owner_lease_selection (
  std::optional<zlink::framework::tests::owner_lease_time_store_t::lease_view_t> lease_view)
{
    using zlink::framework::runtime::provider_location_repository_t;
    using zlink::framework::tests::owner_lease_time_store_t;

    auto inner = std::make_shared<in_memory_location_store_t> ();
    std::shared_ptr<zlink::framework::location_store_t> public_store = inner;
    std::shared_ptr<owner_lease_time_store_t> observed_store;
    if (lease_view) {
        provider_location_repository_t seeder (*inner);
        seeder.configure_reclaim_margin (std::chrono::milliseconds::zero ());
        const auto claimed =
          seeder.claim_owner_lease ("stale-target-owner", std::chrono::seconds (30))
            .result ()
            .value ();
        const auto *owner = std::get_if<zlink::framework::owner_lease_claimed_t> (&claimed);
        if (owner == nullptr)
            throw std::runtime_error ("failed to seed stale target owner lease");
        zlink::framework::mesh_node_descriptor_t descriptor{
          .mesh_name = "lease-target-mesh",
          .rid = zlink::routing_id_t::from ("aaa-stale-target"),
          .lifecycle_generation = 1,
          .descriptor_revision = 1,
          .endpoint = "tcp://127.0.0.1:1",
          .application_version = 1,
          .object_capabilities = {{zlink::framework::placement_object_kind_t::user_spot, "room",
                                   zlink::framework::maintenance_policy_kind_t::disabled, false, 0},
                                  {zlink::framework::placement_object_kind_t::instance_spot, "cart",
                                   zlink::framework::maintenance_policy_kind_t::disabled, false,
                                   0}},
          .object_role = zlink::framework::object_role_t::server,
          .placement_weight = 10000,
          .capacity = {.spots = {.limit = 16},
                       .spot_types = {{zlink::framework::placement_object_kind_t::user_spot,
                                       "room",
                                       {.limit = 16}},
                                      {zlink::framework::placement_object_kind_t::instance_spot,
                                       "cart",
                                       {.limit = 16}}}},
          .activation_concurrency = {.limit = 16},
          .state = zlink::framework::framework_runtime_state_t::serving,
          .security_identity = "test",
          .owner_id = owner->token.owner_id,
          .lease_generation = owner->token.lease_generation};
        const auto stored = seeder.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                              .result ()
                              .value ();
        if (stored.status != location_write_status_t::stored)
            throw std::runtime_error ("failed to seed stale target descriptor");
        observed_store =
          std::make_shared<owner_lease_time_store_t> (*inner, owner->token.owner_id, *lease_view);
        public_store = observed_store;
    }

    const auto user_spot_id = target_id_selected_from_range ("lease-user-", 10001, 1, 10000);
    const auto instance_spot_id = target_id_selected_from_range ("lease-instance-", 2, 0, 1);
    auto app = zlink::framework::app_t::create ();
    auto relocation_store =
      std::make_shared<zlink::framework::runtime::in_memory_relocation_store_t> ();
    descriptor_owner_lease_client_t *client = nullptr;
    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &options) {
        options.configure_locations ().owner_lease_renew_interval = std::chrono::milliseconds (10);
        options.configure_locations ().owner_lease_fencing_margin = std::chrono::milliseconds (1);
        options.add_location_store (public_store);
        options.add_relocation_store (relocation_store);
        options.add_route_mesh ("lease-target-mesh")
          .set_object_role (zlink::framework::object_role_t::server)
          .set_routing_id (zlink::routing_id_t::from ("zzz-live-target"))
          .set_placement_weight (1)
          .listen ("tcp://127.0.0.1:0")
          .add_spot_factory<local_user_spot_t> (
            "room",
            [] (zlink::framework::spot_context_t context) {
                return std::make_shared<local_user_spot_t> (std::move (context));
            },
            [] (auto &factory) { factory.disable_relocation (); })
          .add_instance_spot_factory<lease_target_instance_spot_t> (
            "cart",
            [] (zlink::framework::instance_spot_context_t context) {
                return std::make_shared<lease_target_instance_spot_t> (std::move (context));
            },
            [] (auto &factory) { factory.disable_relocation (); });
    });
    auto service = std::make_unique<descriptor_owner_lease_client_t> (
      app, user_spot_id, instance_spot_id, observed_store);
    client = service.get ();
    app.add_hosted_service (std::move (service));

    // Request submission keeps the caller's flow only while flow capture is on.
    app.set_message_flow_mode (zlink::framework::message_flow_log_mode_t::normal);
    descriptor_owner_lease_result_t result;
    result.app_result = app.run (0, nullptr);
    if (client != nullptr) {
        result.user_target = std::move (client->user_target);
        result.user_error = client->user_error;
        result.user_exception = std::move (client->user_exception);
        result.expired_descriptor_observed = client->expired_descriptor_observed;
        result.discovery_error = client->discovery_error;
        result.instance_target = std::move (client->instance_target);
        result.instance_error = client->instance_error;
        result.instance_exception = std::move (client->instance_exception);
        result.instance_owner_checked_before_authority =
          client->instance_owner_checked_before_authority;
    }
    return result;
}

class user_spot_manager_client_t final : public zlink::framework::hosted_service_t
{
  public:
    explicit user_spot_manager_client_t (zlink::framework::app_t &app) : _app (&app) {}

    zlink::framework::task_t<void> start (zlink::framework::service_provider_t &services) override
    {
        _worker = std::jthread ([this, services] () mutable {
            zlink::framework::test::wait_for_serving (services);
            execute (services).result ().value ();
        });
        co_return;
    }

    zlink::framework::task_t<void> execute (zlink::framework::service_provider_t &services)
    {
        try {
            auto &manager = services.get_required<zlink::framework::spot_manager_t> ();
            const auto rid = zlink::framework::spot_id_t ("local-user-spot");
            auto created = manager.get_or_create (rid, "room")
                             .timeout (std::chrono::seconds (2))
                             .async ()
                             .result ();
            if (!created) {
                last_error =
                  created.error () ? created.error ()->what () : "User Spot create failed";
                _app->stop ();
                co_return;
            }
            auto found = manager.find (rid).result ();
            if (!found || !found.value ()) {
                last_error = found.error () ? found.error ()->what () : "User Spot find failed";
                _app->stop ();
                co_return;
            }
            auto closed = manager.close (*found.value ()).result ();
            if (!closed || !closed.value ()) {
                last_error = closed.error () ? closed.error ()->what () : "User Spot close failed";
                _app->stop ();
                co_return;
            }
            observed = true;
        }
        catch (const std::exception &error) {
            last_error = error.what ();
        }
        _app->stop ();
        co_return;
    }

    void stop () noexcept override {}

    bool observed = false;
    std::string last_error;

  private:
    zlink::framework::app_t *_app;

  private:
    std::jthread _worker;
};

class successive_user_spot_delivery_client_t final : public zlink::framework::hosted_service_t
{
  public:
    explicit successive_user_spot_delivery_client_t (zlink::framework::app_t &app) : _app (&app) {}

    zlink::framework::task_t<void> start (zlink::framework::service_provider_t &services) override
    {
        _worker = std::jthread ([this, services] () mutable {
            zlink::framework::test::wait_for_serving (services);
            execute (services).result ().value ();
        });
        co_return;
    }

    zlink::framework::task_t<void> execute (zlink::framework::service_provider_t &services)
    {
        try {
            auto &manager = services.get_required<zlink::framework::spot_manager_t> ();
            for (const auto *id : {"fence-first", "fence-second"}) {
                const auto created =
                  manager.get_or_create (zlink::framework::spot_id_t (id), "room")
                    .in_mesh ("spot-fence-mesh")
                    .timeout (std::chrono::seconds (2))
                    .async ()
                    .result ();
                if (!created) {
                    last_error =
                      created.error () ? created.error ()->what () : "User Spot create failed";
                    _app->stop ();
                    co_return;
                }
            }

            auto route = _app->advanced ().zlink ().route_client (
              services.get_required<zlink::framework::serializer_registry_t> ());
            const auto submitted = route
                                     .send_to_spot (zlink::framework::spot_id_t ("fence-second"),
                                                    user_spot_delivery_probe_t{42})
                                     .async ()
                                     .result ();
            if (!submitted) {
                last_error = submitted.error () ? submitted.error ()->what ()
                                                : "User Spot delivery submission failed";
                _app->stop ();
                co_return;
            }
            const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
            while (local_user_spot_t::last_delivery.load (std::memory_order_acquire) != 42
                   && std::chrono::steady_clock::now () < deadline) {
                std::this_thread::sleep_for (std::chrono::milliseconds (1));
            }
            observed = local_user_spot_t::last_delivery.load (std::memory_order_acquire) == 42;
            if (!observed)
                last_error = "The second User Spot rejected its current authority fence";
        }
        catch (const std::exception &error) {
            last_error = error.what ();
        }
        _app->stop ();
        co_return;
    }

    void stop () noexcept override {}

    bool observed = false;
    std::string last_error;

  private:
    zlink::framework::app_t *_app;

  private:
    std::jthread _worker;
};

TEST (ZLinkFrameworkStoreLocationResolvers, PublicSpotManagerUsesLocationReservationAndMeshCommands)
{
    auto store = std::make_shared<in_memory_location_store_t> ();
    auto app = zlink::framework::app_t::create ();
    user_spot_manager_client_t *client = nullptr;

    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &options) {
        options.configure_locations ().owner_lease_renew_interval = std::chrono::milliseconds (10);
        options.configure_locations ().owner_lease_fencing_margin = std::chrono::milliseconds (1);
        options.add_location_store (store);
        auto node = options.add_route_mesh ("spot-mesh");
        node.set_object_role (zlink::framework::object_role_t::server)
          .set_routing_id (zlink::routing_id_t::from ("spot-local-node"))
          .listen ("tcp://127.0.0.1:0")
          .add_spot_factory<local_user_spot_t> (
            "room",
            [] (zlink::framework::spot_context_t context) {
                return std::make_shared<local_user_spot_t> (std::move (context));
            },
            [] (auto &factory) { factory.disable_relocation (); });
    });
    auto service = std::make_unique<user_spot_manager_client_t> (app);
    client = service.get ();
    app.add_hosted_service (std::move (service));

    EXPECT_EQ (0, app.run (0, nullptr));
    ASSERT_NE (nullptr, client);
    EXPECT_TRUE (client->observed) << client->last_error;
}

TEST (ZLinkFrameworkStoreLocationResolvers, SuccessiveUserSpotAcceptsItsCurrentAuthorityFence)
{
    local_user_spot_t::last_delivery.store (0, std::memory_order_release);
    auto store = std::make_shared<in_memory_location_store_t> ();
    auto app = zlink::framework::app_t::create ();
    successive_user_spot_delivery_client_t *client = nullptr;

    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &options) {
        options.configure_locations ().owner_lease_renew_interval = std::chrono::milliseconds (10);
        options.configure_locations ().owner_lease_fencing_margin = std::chrono::milliseconds (1);
        options.add_location_store (store);
        options.add_route_mesh ("spot-fence-mesh")
          .set_object_role (zlink::framework::object_role_t::server)
          .set_routing_id (zlink::routing_id_t::from ("spot-fence-node"))
          .listen ("tcp://127.0.0.1:0")
          .add_spot_factory<local_user_spot_t> (
            "room",
            [] (zlink::framework::spot_context_t context) {
                return std::make_shared<local_user_spot_t> (std::move (context));
            },
            [] (auto &factory) { factory.disable_relocation (); });
    });
    auto service = std::make_unique<successive_user_spot_delivery_client_t> (app);
    client = service.get ();
    app.add_hosted_service (std::move (service));

    EXPECT_EQ (0, app.run (0, nullptr));
    ASSERT_NE (nullptr, client);
    EXPECT_TRUE (client->observed) << client->last_error;
}

TEST (ZLinkFrameworkStoreLocationResolvers,
      ExpiredDescriptorOwnerIsSkippedForUserAndInstanceSpotTargets)
{
    using lease_view_t = zlink::framework::tests::owner_lease_time_store_t::lease_view_t;
    const auto result = run_descriptor_owner_lease_selection (lease_view_t::expired);

    EXPECT_EQ (0, result.app_result);
    EXPECT_EQ (std::optional<std::string> ("zzz-live-target"), result.user_target)
      << result.user_exception;
    EXPECT_FALSE (result.user_error.has_value ());
    EXPECT_TRUE (result.expired_descriptor_observed);
    EXPECT_EQ (std::optional<std::string> ("zzz-live-target"), result.instance_target)
      << result.instance_exception;
    EXPECT_FALSE (result.instance_error.has_value ());
    EXPECT_EQ (std::optional<bool> (true), result.instance_owner_checked_before_authority);
}

TEST (ZLinkFrameworkStoreLocationResolvers,
      LiveDescriptorOwnerRemainsEligibleForUserAndInstanceSpotTargets)
{
    const auto result = run_descriptor_owner_lease_selection (std::nullopt);

    EXPECT_EQ (0, result.app_result);
    EXPECT_EQ (std::optional<std::string> ("zzz-live-target"), result.user_target)
      << result.user_exception;
    EXPECT_FALSE (result.user_error.has_value ());
    EXPECT_EQ (std::optional<std::string> ("zzz-live-target"), result.instance_target)
      << result.instance_exception;
    EXPECT_FALSE (result.instance_error.has_value ());
}

TEST (ZLinkFrameworkStoreLocationResolvers,
      MissingDescriptorOwnerExpiryFailsUserAndInstanceSpotTargetsInternally)
{
    using zlink::framework::framework_error_kind_t;
    using lease_view_t = zlink::framework::tests::owner_lease_time_store_t::lease_view_t;
    const auto result = run_descriptor_owner_lease_selection (lease_view_t::missing_expiry);

    EXPECT_EQ (0, result.app_result);
    EXPECT_FALSE (result.user_target.has_value ());
    EXPECT_EQ (std::optional<framework_error_kind_t> (framework_error_kind_t::internal_failure),
               result.user_error)
      << result.user_exception;
    EXPECT_EQ (std::optional<framework_error_kind_t> (framework_error_kind_t::internal_failure),
               result.discovery_error);
    EXPECT_FALSE (result.instance_target.has_value ());
    EXPECT_EQ (std::optional<framework_error_kind_t> (framework_error_kind_t::internal_failure),
               result.instance_error)
      << result.instance_exception;
    EXPECT_EQ (std::optional<bool> (true), result.instance_owner_checked_before_authority);
}

TEST (ZLinkFrameworkStoreLocationResolvers, ContextOnlySpotFactoryReceivesExactFrameworkIdentity)
{
    auto store = std::make_shared<in_memory_location_store_t> ();
    auto app = zlink::framework::app_t::create ();
    user_spot_manager_client_t *client = nullptr;
    std::string observed_mesh;
    std::string observed_node;
    std::string observed_spot;
    std::uint64_t observed_generation = 0;
    auto destruction_count = std::make_shared<std::atomic<int>> (0);

    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &options) {
        options.configure_locations ().owner_lease_renew_interval = std::chrono::milliseconds (10);
        options.configure_locations ().owner_lease_fencing_margin = std::chrono::milliseconds (1);
        options.add_location_store (store);
        auto node = options.add_route_mesh ("context-mesh");
        node.set_object_role (zlink::framework::object_role_t::server)
          .set_routing_id (zlink::routing_id_t::from ("context-node"))
          .listen ("tcp://127.0.0.1:0")
          .add_spot_factory<context_owned_user_spot_t> (
            "room",
            [&] (zlink::framework::spot_context_t context) {
                observed_mesh = context.mesh_name ();
                observed_node = std::string (context.node_rid ().value ());
                observed_spot = context.spot_id ();
                observed_generation = context.object_generation ();
                return std::make_shared<context_owned_user_spot_t> (std::move (context),
                                                                    destruction_count);
            },
            [] (auto &factory) { factory.disable_relocation (); });
    });
    auto service = std::make_unique<user_spot_manager_client_t> (app);
    client = service.get ();
    app.add_hosted_service (std::move (service));

    EXPECT_EQ (0, app.run (0, nullptr));
    ASSERT_NE (nullptr, client);
    EXPECT_TRUE (client->observed) << client->last_error;
    EXPECT_EQ ("context-mesh", observed_mesh);
    EXPECT_EQ ("context-node", observed_node);
    EXPECT_EQ ("local-user-spot", observed_spot);
    EXPECT_EQ (1u, observed_generation);
    EXPECT_EQ (1, destruction_count->load (std::memory_order_relaxed))
      << "host teardown must release the Spot instance/context ownership cycle";
}
} // namespace
