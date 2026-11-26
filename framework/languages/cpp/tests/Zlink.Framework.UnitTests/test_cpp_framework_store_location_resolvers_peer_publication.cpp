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
#include "../support/store_location_resolver_channel_fixture.hpp"

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

class standalone_framework_runtime_t final : public zlink::framework::framework_runtime_t
{
  public:
    zlink::framework::framework_runtime_status_t status () const override
    {
        return {.state = zlink::framework::framework_runtime_state_t::serving,
                .is_ready = true,
                .accepting_work = true};
    }

    void reset_capacity_metrics () override
    {
        ADD_FAILURE () << "unexpected standalone host capacity reset";
    }

    zlink::framework::listener_status_t listener_status (zlink::framework::listener_kind_t,
                                                         std::string) const override
    {
        ADD_FAILURE () << "unexpected standalone host listener status query";
        return {};
    }

    std::vector<zlink::framework::http_listener_status_t> http_listener_statuses () const override
    {
        ADD_FAILURE () << "unexpected standalone host HTTP listener status query";
        return {};
    }

    std::unique_ptr<zlink::framework::runtime_observation_t>
    observe (std::size_t,
             std::function<void (const zlink::framework::observed_status_t<
                                 zlink::framework::framework_runtime_status_t> &)>) override
    {
        ADD_FAILURE () << "unexpected standalone host observation";
        return {};
    }
};
} // namespace

namespace
{
class automatic_scoped_request_handler_t
{
  public:
    using request_type = auto_connect_request_t;
    using reply_type = auto_connect_reply_t;

    explicit automatic_scoped_request_handler_t (automatic_handler_scope_dependency_t &dependency) :
        _dependency (dependency)
    {
    }

    auto_connect_reply_t handle (const auto_connect_request_t &request)
    {
        same_scope.store (
          automatic_handler_scope_filter_t::last_filter_dependency.load (std::memory_order_acquire)
            == &_dependency,
          std::memory_order_release);
        ++invocations;
        return auto_connect_reply_t{request.value + 1000};
    }

    inline static std::atomic_bool same_scope{false};
    inline static std::atomic_int invocations{0};

  private:
    automatic_handler_scope_dependency_t &_dependency;
};

class auto_connect_request_client_t final : public zlink::framework::hosted_service_t
{
  public:
    explicit auto_connect_request_client_t (zlink::framework::app_t &app) : _app (&app) {}

    zlink::framework::task_t<void> start (zlink::framework::service_provider_t &) override
    {
        auto client = _app->advanced ().zlink ().request_client ("orders");
        for (int attempt = 0; attempt < 40; ++attempt) {
            auto reply = client.request (auto_connect_request_t{17})
                           .timeout (std::chrono::milliseconds (500))
                           .async<auto_connect_reply_t> ()
                           .result ();
            if (reply && reply.value ().value == 1017) {
                observed = true;
                break;
            }
            if (!reply && reply.error ()) {
                last_error = reply.error ()->what ();
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (25));
        }
        _app->stop ();
        co_return;
    }

    void stop () noexcept override {}

    bool observed = false;
    std::string last_error;

  private:
    zlink::framework::app_t *_app;
};

class missing_auto_connect_request_client_t final : public zlink::framework::hosted_service_t
{
  public:
    explicit missing_auto_connect_request_client_t (zlink::framework::app_t &app) : _app (&app) {}
    zlink::framework::task_t<void> start (zlink::framework::service_provider_t &) override
    {
        auto reply = _app->advanced ()
                       .zlink ()
                       .request_client ("orders")
                       .request (auto_connect_request_t{17})
                       .timeout (std::chrono::milliseconds (50))
                       .async<auto_connect_reply_t> ()
                       .result ();
        if (!reply) {
            observed_error = reply.error_kind ();
        }
        _app->stop ();
        co_return;
    }

    void stop () noexcept override {}

    std::optional<zlink::framework::framework_error_kind_t> observed_error;

  private:
    zlink::framework::app_t *_app;
};

class rejected_auto_connect_request_client_t final : public zlink::framework::hosted_service_t
{
  public:
    explicit rejected_auto_connect_request_client_t (zlink::framework::app_t &app) : _app (&app) {}

    zlink::framework::task_t<void> start (zlink::framework::service_provider_t &) override
    {
        auto client = _app->advanced ().zlink ().request_client ("orders");
        for (int attempt = 0; attempt < 40; ++attempt) {
            auto reply = client.request (auto_connect_request_t{17})
                           .timeout (std::chrono::milliseconds (500))
                           .async<auto_connect_reply_t> ()
                           .result ();
            if (!reply
                && reply.error_kind () == zlink::framework::framework_error_kind_t::rejected) {
                observed_rejected = true;
                break;
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (25));
        }
        _app->stop ();
        co_return;
    }

    void stop () noexcept override {}

    bool observed_rejected = false;

  private:
    zlink::framework::app_t *_app;
};

TEST (ZLinkFrameworkStoreLocationResolvers, AutoConnectHostPublishesAndCleansLocalPeers)
{
    auto store = std::make_shared<in_memory_location_repository_t> ();
    location_options_t options;
    options.owner_lease_renew_interval = std::chrono::milliseconds (50);
    auto runtime = std::make_shared<location_runtime_t> (*store, options, "owner-auto");
    runtime->start (zlink::routing_id_t::from ("node-auto"));

    zlink::framework::zlink_builder_t zlink = zlink::framework::test::runtime_failure_builder ();
    auto orders = zlink.channel ("orders");
    orders.enable_server ()
      .set_routing_id (zlink::routing_id_t::from ("orders-router"))
      .peer_weight (zlink::peer_weight_t::value (7))
      .bind ("tcp://127.0.0.1:0");
    orders.enable_client ();
    auto events = zlink.channel ("events");
    events.enable_publisher ()
      .set_routing_id (zlink::routing_id_t::from ("events-pub"))
      .bind ("tcp://127.0.0.1:0");
    events.enable_subscriber ();

    zlink::framework::service_collection_t services;
    services.add_factory<zlink::framework::framework_runtime_t> (
      [] (zlink::framework::service_provider_t &)
        -> std::shared_ptr<zlink::framework::framework_runtime_t> {
          return std::make_shared<standalone_framework_runtime_t> ();
      },
      zlink::framework::service_lifetime_t::singleton);
    services.add_factory<zlink::framework::location_repository_t> (
      [store] (zlink::framework::service_provider_t &) {
          return std::static_pointer_cast<zlink::framework::location_repository_t> (store);
      },
      zlink::framework::service_lifetime_t::singleton);
    services.add_factory<zlink::framework::runtime::live_location_reader_t> (
      [store, options] (zlink::framework::service_provider_t &) {
          return std::make_shared<zlink::framework::runtime::live_location_reader_t> (*store,
                                                                                      options);
      },
      zlink::framework::service_lifetime_t::singleton);
    services.add_factory<location_runtime_t> (
      [runtime] (zlink::framework::service_provider_t &) { return runtime; },
      zlink::framework::service_lifetime_t::singleton);
    auto provider = services.build_provider ();

    zlink::framework::handler_registry_t handlers;
    zlink::framework::serializer_registry_t serializers;
    location_auto_connect_host_service_t service (
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> (),
      zlink.message_bus (),
      zlink::framework::detail::channel_runtime_t::from (zlink.message_bus ()).channel_snapshots (),
      handlers, serializers, [&provider] {
          return provider.get_required<zlink::framework::framework_runtime_t> ().status ().state;
      });
    service.start (provider);

    const auto client_servers = store->list_client_servers ("orders").result ().value ();
    ASSERT_EQ (1u, client_servers.items.size ());
    EXPECT_EQ ("orders-router", client_servers.items.front ().server_rid.to_string ());
    EXPECT_TRUE (client_servers.items.front ().endpoint.starts_with ("tcp://127.0.0.1:"));
    EXPECT_NE ("tcp://127.0.0.1:0", client_servers.items.front ().endpoint);
    EXPECT_EQ (7, client_servers.items.front ().weight);
    const auto fanout_publishers = store->list_fanout_publishers ("events").result ().value ();
    ASSERT_EQ (1u, fanout_publishers.items.size ());
    EXPECT_EQ ("events", fanout_publishers.items.front ().channel_name);
    EXPECT_EQ ("events-pub", fanout_publishers.items.front ().publisher_rid.to_string ());
    EXPECT_TRUE (fanout_publishers.items.front ().endpoint.starts_with ("tcp://127.0.0.1:"));
    EXPECT_NE ("tcp://127.0.0.1:0", fanout_publishers.items.front ().endpoint);
    EXPECT_EQ (zlink::framework::framework_runtime_state_t::serving,
               fanout_publishers.items.front ().state);

    service.stop ();
    const auto remaining_client_servers = store->list_client_servers ("orders").result ().value ();
    EXPECT_TRUE (remaining_client_servers.items.empty ())
      << "remaining owner="
      << (remaining_client_servers.items.empty ()
            ? std::string ("<none>")
            : remaining_client_servers.items.front ().owner_id)
      << " generation="
      << (remaining_client_servers.items.empty ()
            ? 0
            : remaining_client_servers.items.front ().lease_generation);
    EXPECT_TRUE (store->list_fanout_publishers ("events").result ().value ().items.empty ());
    runtime->stop ();
}

TEST (ZLinkFrameworkStoreLocationResolvers, ClientServerRuntimeStateMappingMatchesServiceWire)
{
    using zlink::framework::framework_runtime_state_t;
    using zlink::framework::runtime::client_server::client_server_framework_state;
    using zlink::framework::runtime::client_server::client_server_service_state;
    using zlink::framework::runtime::mesh::service_node_state_t;

    const auto round_trip = [&] (framework_runtime_state_t state) {
        return client_server_framework_state (client_server_service_state (state));
    };

    EXPECT_EQ (framework_runtime_state_t::preparing,
               round_trip (framework_runtime_state_t::preparing));
    EXPECT_EQ (framework_runtime_state_t::serving, round_trip (framework_runtime_state_t::serving));
    EXPECT_EQ (framework_runtime_state_t::draining,
               round_trip (framework_runtime_state_t::relocating));
    EXPECT_EQ (framework_runtime_state_t::draining,
               round_trip (framework_runtime_state_t::relocated));
    EXPECT_EQ (framework_runtime_state_t::draining,
               round_trip (framework_runtime_state_t::draining));
    EXPECT_EQ (framework_runtime_state_t::stopped, round_trip (framework_runtime_state_t::stopped));
    EXPECT_EQ (framework_runtime_state_t::error, round_trip (framework_runtime_state_t::error));
    EXPECT_EQ (framework_runtime_state_t::relocating,
               client_server_framework_state (service_node_state_t::retiring));
}

TEST (ZLinkFrameworkStoreLocationResolvers,
      StoreTransportIdentityMapsToServiceWireAdmissionIdentity)
{
    using zlink::framework::runtime::location_auto_connect_detail::
      to_service_wire_admission_identity;

    EXPECT_EQ ("default", to_service_wire_admission_identity ("plaintext"));
    EXPECT_EQ ("default", to_service_wire_admission_identity ("default"));
    EXPECT_EQ ("tls:cluster-a", to_service_wire_admission_identity ("tls:cluster-a"));
}

TEST (ZLinkFrameworkStoreLocationResolvers,
      AppClientServerDefaultConfigAdmissionUsesLocationAutoConnect)
{
    auto store = std::make_shared<in_memory_location_store_t> ();
    auto app = zlink::framework::app_t::create ();
    auto_connect_request_client_t *client = nullptr;

    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &options) {
        options.add_location_store (store);
        options.handlers ().group ("orders").add<auto_connect_request_handler_t> ();
        options.add_client_server_channel ("orders")
          .server ()
          .set_bind_host ("127.0.0.1")
          .listen (0)
          .add_handler_group ("orders");
        options.add_client_server_channel ("orders").client ();
    });
    auto service = std::make_unique<auto_connect_request_client_t> (app);
    client = service.get ();
    app.add_hosted_service (std::move (service));

    EXPECT_EQ (0, app.run (0, nullptr));
    ASSERT_NE (nullptr, client);
    EXPECT_TRUE (client->observed) << client->last_error;
}

TEST (ZLinkFrameworkStoreLocationResolvers,
      ClientServerWithoutReadySnapshotExpiresAtAdmissionDeadline)
{
    auto store = std::make_shared<in_memory_location_store_t> ();
    auto app = zlink::framework::app_t::create ();
    missing_auto_connect_request_client_t *client = nullptr;

    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &options) {
        options.add_location_store (store);
        options.add_client_server_channel ("orders").client ();
    });
    auto service = std::make_unique<missing_auto_connect_request_client_t> (app);
    client = service.get ();
    app.add_hosted_service (std::move (service));

    EXPECT_EQ (0, app.run (0, nullptr));
    ASSERT_NE (nullptr, client);
    ASSERT_TRUE (client->observed_error.has_value ());
    EXPECT_EQ (zlink::framework::framework_error_kind_t::deadline_exceeded,
               *client->observed_error);
}

TEST (ZLinkFrameworkStoreLocationResolvers,
      SameProcessClientServerUsesLocalReadyServerWithoutExternalStoreOrManualEndpoint)
{
    automatic_handler_scope_dependency_t::created.store (0);
    automatic_handler_scope_dependency_t::destroyed.store (0);
    automatic_handler_scope_filter_t::reject_requests.store (false);
    automatic_handler_scope_filter_t::last_filter_dependency.store (nullptr);
    automatic_scoped_request_handler_t::same_scope.store (false);
    automatic_scoped_request_handler_t::invocations.store (0);
    auto app = zlink::framework::app_t::create ();
    auto_connect_request_client_t *client = nullptr;

    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &options) {
        options.add_location_store (
          std::make_shared<zlink::framework::runtime::in_memory_location_store_t> ());
        options.handlers ().group ("orders").add<automatic_scoped_request_handler_t> ();
        options.services ().add_scoped<automatic_handler_scope_dependency_t> ();
        options.use_filter<automatic_handler_scope_filter_t> ();
        auto channel = options.add_client_server_channel ("orders");
        channel.server ()
          .set_bind_host ("127.0.0.1")
          .set_advertise_host ("127.0.0.1")
          .set_weight (7)
          .listen ()
          .add_handler_group ("orders");
        channel.client ();
    });
    auto service = std::make_unique<auto_connect_request_client_t> (app);
    client = service.get ();
    app.add_hosted_service (std::move (service));

    EXPECT_EQ (0, app.run (0, nullptr));
    ASSERT_NE (nullptr, client);
    EXPECT_TRUE (client->observed) << client->last_error;
    EXPECT_TRUE (automatic_scoped_request_handler_t::same_scope.load ());
    EXPECT_EQ (1, automatic_scoped_request_handler_t::invocations.load ());
    EXPECT_EQ (automatic_handler_scope_dependency_t::created.load (),
               automatic_handler_scope_dependency_t::destroyed.load ());
}

TEST (ZLinkFrameworkStoreLocationResolvers,
      SameProcessClientServerFilterRejectionReturnsTypedErrorAndDisposesScope)
{
    automatic_handler_scope_dependency_t::created.store (0);
    automatic_handler_scope_dependency_t::destroyed.store (0);
    automatic_handler_scope_filter_t::reject_requests.store (true);
    automatic_handler_scope_filter_t::last_filter_dependency.store (nullptr);
    automatic_scoped_request_handler_t::same_scope.store (false);
    automatic_scoped_request_handler_t::invocations.store (0);
    auto app = zlink::framework::app_t::create ();
    rejected_auto_connect_request_client_t *client = nullptr;

    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &options) {
        options.add_location_store (
          std::make_shared<zlink::framework::runtime::in_memory_location_store_t> ());
        options.handlers ().group ("orders").add<automatic_scoped_request_handler_t> ();
        options.services ().add_scoped<automatic_handler_scope_dependency_t> ();
        options.use_filter<automatic_handler_scope_filter_t> ();
        auto channel = options.add_client_server_channel ("orders");
        channel.server ()
          .set_bind_host ("127.0.0.1")
          .set_advertise_host ("127.0.0.1")
          .listen ()
          .add_handler_group ("orders");
        channel.client ();
    });
    auto service = std::make_unique<rejected_auto_connect_request_client_t> (app);
    client = service.get ();
    app.add_hosted_service (std::move (service));

    EXPECT_EQ (0, app.run (0, nullptr));
    ASSERT_NE (nullptr, client);
    EXPECT_TRUE (client->observed_rejected);
    EXPECT_EQ (0, automatic_scoped_request_handler_t::invocations.load ());
    EXPECT_GT (automatic_handler_scope_dependency_t::created.load (), 0);
    EXPECT_EQ (automatic_handler_scope_dependency_t::created.load (),
               automatic_handler_scope_dependency_t::destroyed.load ());
    automatic_handler_scope_filter_t::reject_requests.store (false);
}
} // namespace
