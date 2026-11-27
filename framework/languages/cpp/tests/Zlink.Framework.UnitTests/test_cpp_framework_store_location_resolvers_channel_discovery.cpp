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
#include "../support/store_location_resolver_repository_fixture.hpp"
#include "../support/store_location_resolver_channel_fixture.hpp"

namespace zlink::framework::runtime
{
struct location_auto_connect_test_access_t
{
    static std::vector<std::string>
    desired_rids (std::shared_ptr<const live_location_reader_t::mesh_sources_t> sources)
    {
        location_auto_connect_host_service_t::loop_t local;
        local.mesh_name = "mesh";
        local.local_rid = zlink::routing_id_t::from ("local");
        local.local_endpoint = "tcp://shared:1";
        std::vector<std::string> rids;
        for (const auto &[key, target] :
             location_auto_connect_host_service_t::compute_desired (local, std::move (sources)))
            rids.push_back (target.node_rid.to_string ());
        return rids;
    }
};
} // namespace zlink::framework::runtime

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

struct auto_connect_event_t
{
    int value{};
};
} // namespace

template <> struct zlink::json::dto_traits<auto_connect_event_t>
{
    static constexpr auto fields =
      std::tuple{zlink::json::field ("value", &auto_connect_event_t::value)};
};

namespace
{
class auto_connect_event_handler_t
{
  public:
    using event_type = auto_connect_event_t;
    static constexpr const char *topic_name = "profile.changed";

    explicit auto_connect_event_handler_t (automatic_handler_scope_dependency_t &dependency) :
        _dependency (dependency)
    {
    }

    static inline std::atomic<int> observed_count{0};
    static inline std::atomic<int> observed_value{0};

    void handle (const auto_connect_event_t &event)
    {
        same_scope.store (
          automatic_handler_scope_filter_t::last_filter_dependency.load (std::memory_order_acquire)
            == &_dependency,
          std::memory_order_release);
        observed_value.store (event.value, std::memory_order_release);
        observed_count.fetch_add (1, std::memory_order_acq_rel);
    }

    static inline std::atomic_bool same_scope{false};

  private:
    automatic_handler_scope_dependency_t &_dependency;
};

class blocked_auto_connect_event_handler_t
{
  public:
    using event_type = blocked_auto_connect_event_t;
    static constexpr const char *topic_name = "profile.changed";

    explicit blocked_auto_connect_event_handler_t (automatic_handler_scope_dependency_t &) {}

    void handle (const blocked_auto_connect_event_t &) { ++observed_count; }

    static inline std::atomic_int observed_count{0};
};

class auto_connect_publish_client_t final : public zlink::framework::hosted_service_t
{
  public:
    explicit auto_connect_publish_client_t (zlink::framework::app_t &app) : _app (&app) {}

    zlink::framework::task_t<void> start (zlink::framework::service_provider_t &) override
    {
        auto publisher = _app->advanced ().zlink ().publisher ();
        for (int attempt = 0; attempt < 80; ++attempt) {
            try {
                co_await publisher
                  .publish ("events", "profile.changed", blocked_auto_connect_event_t{attempt + 1})
                  .async ();
            }
            catch (const std::exception &error) {
                last_error = error.what ();
            }
            if (automatic_handler_scope_filter_t::blocked_fanout_dispatches.load (
                  std::memory_order_acquire)
                > 0)
                break;
            std::this_thread::sleep_for (std::chrono::milliseconds (25));
        }
        for (int attempt = 0; attempt < 80; ++attempt) {
            try {
                co_await publisher
                  .publish ("events", "profile.changed", auto_connect_event_t{attempt + 1})
                  .async ();
            }
            catch (const std::exception &error) {
                last_error = error.what ();
            }
            if (auto_connect_event_handler_t::observed_count.load (std::memory_order_acquire) > 0) {
                observed = true;
                break;
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

class fanout_topic_context_handler_t
{
  public:
    using event_type = auto_connect_event_t;

    void handle (const auto_connect_event_t &event,
                 const zlink::framework::publish_message_context_t &context)
    {
        if (event.value == 1 && context.topic == "a") {
            observed_a.store (true, std::memory_order_release);
        } else if (event.value == 2 && context.topic == "b") {
            observed_b.store (true, std::memory_order_release);
        }
    }

    inline static std::atomic_bool observed_a{false};
    inline static std::atomic_bool observed_b{false};
};

class fanout_topic_context_publish_client_t final : public zlink::framework::hosted_service_t
{
  public:
    explicit fanout_topic_context_publish_client_t (zlink::framework::app_t &app) : _app (&app) {}

    zlink::framework::task_t<void> start (zlink::framework::service_provider_t &) override
    {
        auto publisher = _app->advanced ().zlink ().publisher ();
        for (int attempt = 0;
             attempt < 80
             && (!fanout_topic_context_handler_t::observed_a.load (std::memory_order_acquire)
                 || !fanout_topic_context_handler_t::observed_b.load (std::memory_order_acquire));
             ++attempt) {
            try {
                if (!fanout_topic_context_handler_t::observed_a.load (std::memory_order_acquire)) {
                    co_await publisher
                      .publish ("fanout-topic-context", "a", auto_connect_event_t{1})
                      .async ();
                }
                if (!fanout_topic_context_handler_t::observed_b.load (std::memory_order_acquire)) {
                    co_await publisher
                      .publish ("fanout-topic-context", "b", auto_connect_event_t{2})
                      .async ();
                }
            }
            catch (const std::exception &error) {
                last_error = error.what ();
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (25));
        }
        _app->stop ();
        co_return;
    }

    void stop () noexcept override {}

    std::string last_error;

  private:
    zlink::framework::app_t *_app;
};

TEST (ZLinkFrameworkStoreLocationResolvers, ClientServerPortZeroPublishesAdvertiseHostWithBoundPort)
{
    namespace client_server = zlink::framework::runtime::client_server;
    namespace protocol = zlink::framework::runtime::protocol;
    namespace mesh = zlink::framework::runtime::mesh;

    client_server::raw_client_server_server_t server (
      zlink::framework::test::runtime_failure_options (
        client_server::raw_client_server_server_options_t{
          protocol::client_server_server_admission_t{
            .channel_name = "orders",
            .server_routing_id = {'o', 'r', 'd', 'e', 'r', 's'},
            .lifecycle_generation = 1,
            .weight = 100,
            .state = mesh::service_node_state_t::preparing,
            .security_identity = "test",
            .effective_max_message_bytes = 1024,
            .advertised_endpoint = "tcp://127.0.0.1:*"},
          std::string ("service.example")}));
    server.start ();
    const auto endpoint = server.endpoint ();
    EXPECT_TRUE (endpoint.starts_with ("tcp://service.example:"));
    EXPECT_EQ (std::string::npos, endpoint.find ('*'));
    EXPECT_GT (std::stoi (endpoint.substr (endpoint.rfind (':') + 1)), 0);
    server.close ();
}

TEST (ZLinkFrameworkStoreLocationResolvers,
      ClientServerDuplicateRoleRegistrationFailsBeforeSocketBind)
{
    EXPECT_THROW (
      {
          auto app = zlink::framework::app_t::create ();
          app.add_zlink_framework ([] (zlink::framework::zlink_framework_options_t &options) {
              auto channel = options.add_client_server_channel ("orders");
              channel.client ().connect ("tcp://127.0.0.1:29711");
              channel.client ().connect ("tcp://127.0.0.1:29712");
          });
      },
      zlink::framework::framework_exception_t);

    EXPECT_THROW (
      {
          auto app = zlink::framework::app_t::create ();
          app.add_zlink_framework ([] (zlink::framework::zlink_framework_options_t &options) {
              options.handlers ().group ("orders").add<auto_connect_request_handler_t> ();
              auto channel = options.add_client_server_channel ("orders");
              channel.server ()
                .set_bind_host ("127.0.0.1")
                .listen (29713)
                .add_handler_group ("orders");
              channel.server ().set_bind_host ("127.0.0.1").listen (29714);
          });
      },
      zlink::framework::framework_exception_t);
}

TEST (ZLinkFrameworkStoreLocationResolvers, ClientServerAllowsSameRoleOnDifferentChannelNames)
{
    EXPECT_NO_THROW ({
        auto app = zlink::framework::app_t::create ();
        app.add_zlink_framework ([] (zlink::framework::zlink_framework_options_t &options) {
            options.add_client_server_channel ("orders").client ().connect (
              "tcp://127.0.0.1:29721");
            options.add_client_server_channel ("billing").client ().connect (
              "tcp://127.0.0.1:29722");
        });
    });

    EXPECT_NO_THROW ({
        auto app = zlink::framework::app_t::create ();
        app.add_zlink_framework ([] (zlink::framework::zlink_framework_options_t &options) {
            options.handlers ().group ("orders").add<auto_connect_request_handler_t> ();
            options.add_client_server_channel ("orders")
              .server ()
              .set_bind_host ("127.0.0.1")
              .listen (29723)
              .add_handler_group ("orders");
            options.add_client_server_channel ("billing")
              .server ()
              .set_bind_host ("127.0.0.1")
              .listen (29724)
              .add_handler_group ("orders");
        });
    });
}

TEST (ZLinkFrameworkStoreLocationResolvers,
      RouteMeshAndClientServerChannelNameCollisionFailsInEitherBindOrder)
{
    EXPECT_THROW (
      {
          auto app = zlink::framework::app_t::create ();
          app.add_zlink_framework ([] (zlink::framework::zlink_framework_options_t &options) {
              options.add_route_mesh ("mesh-a").channel_name ("orders").client ();
              options.add_client_server_channel ("orders").client ().connect (
                "tcp://127.0.0.1:29715");
          });
      },
      zlink::framework::framework_exception_t);

    const auto mesh_send = [] (zlink::framework::runtime::messaging::message_parts_t)
      -> zlink::framework::task_t<zlink::framework::result_t<void>> {
        co_return zlink::framework::result_t<void>::success ();
    };
    const auto mesh_request = [] (zlink::framework::runtime::messaging::message_parts_t parts,
                                  std::chrono::milliseconds)
      -> zlink::framework::request_task_t<
        zlink::framework::result_t<zlink::framework::runtime::messaging::message_parts_t>> {
        co_await zlink::framework::detail::request_admitted_t{};
        co_return zlink::framework::result_t<
          zlink::framework::runtime::messaging::message_parts_t>::success (std::move (parts));
    };
    const auto client_server_send =
      [] (std::string, std::string, zlink::message_t,
          std::map<std::string, std::string>) -> zlink::framework::task_t<void> { co_return; };
    const auto client_server_request =
      [] (
        std::string, std::string, zlink::message_t message, std::chrono::milliseconds,
        std::map<std::string, std::string>) -> zlink::framework::request_task_t<zlink::message_t> {
        co_await zlink::framework::detail::request_admitted_t{};
        co_return std::move (message);
    };

    zlink::framework::zlink_builder_t mesh_first_builder =
      zlink::framework::test::runtime_failure_builder ();
    auto mesh_first =
      zlink::framework::detail::channel_runtime_t::from (mesh_first_builder.message_bus ());
    mesh_first.bind_mesh_channel_transport ("orders", mesh_send, mesh_request);
    EXPECT_THROW (
      mesh_first.bind_client_server_transport ("orders", client_server_send, client_server_request),
      zlink::framework::framework_exception_t);

    zlink::framework::zlink_builder_t client_server_first_builder =
      zlink::framework::test::runtime_failure_builder ();
    auto client_server_first = zlink::framework::detail::channel_runtime_t::from (
      client_server_first_builder.message_bus ());
    client_server_first.bind_client_server_transport ("orders", client_server_send,
                                                      client_server_request);
    EXPECT_THROW (
      client_server_first.bind_mesh_channel_transport ("orders", mesh_send, mesh_request),
      zlink::framework::framework_exception_t);
}

TEST (ZLinkFrameworkStoreLocationResolvers, AppFanoutPublishUsesLocationAutoConnect)
{
    auto store = std::make_shared<in_memory_location_store_t> ();
    auto app = zlink::framework::app_t::create ();
    auto_connect_publish_client_t *client = nullptr;
    auto_connect_event_handler_t::observed_count.store (0, std::memory_order_release);
    auto_connect_event_handler_t::observed_value.store (0, std::memory_order_release);
    auto_connect_event_handler_t::same_scope.store (false);
    blocked_auto_connect_event_handler_t::observed_count.store (0);
    automatic_handler_scope_dependency_t::created.store (0);
    automatic_handler_scope_dependency_t::destroyed.store (0);
    automatic_handler_scope_filter_t::reject_requests.store (false);
    automatic_handler_scope_filter_t::last_filter_dependency.store (nullptr);
    automatic_handler_scope_filter_t::blocked_fanout_dispatches.store (0);

    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &options) {
        options.add_location_store (store);
        options.handlers ()
          .group ("events")
          .add_publish<auto_connect_event_handler_t> ()
          .add_publish<blocked_auto_connect_event_handler_t> ();
        options.services ().add_scoped<automatic_handler_scope_dependency_t> ();
        options.use_filter<automatic_handler_scope_filter_t> ();
        options.add_fanout_channel ("events")
          .set_routing_id (zlink::routing_id_t::from ("events-publisher"))
          .enable_publisher ("tcp://127.0.0.1:0")
          .enable_subscriber ()
          .use_handler_group ("events");
    });
    auto service = std::make_unique<auto_connect_publish_client_t> (app);
    client = service.get ();
    app.add_hosted_service (std::move (service));

    EXPECT_EQ (0, app.run (0, nullptr));
    ASSERT_NE (nullptr, client);
    EXPECT_TRUE (client->observed) << client->last_error;
    EXPECT_GT (auto_connect_event_handler_t::observed_value.load (std::memory_order_acquire), 0);
    EXPECT_TRUE (client->last_error.empty ()) << client->last_error;
    EXPECT_TRUE (auto_connect_event_handler_t::same_scope.load ());
    EXPECT_EQ (0, blocked_auto_connect_event_handler_t::observed_count.load ());
    EXPECT_GT (automatic_handler_scope_filter_t::blocked_fanout_dispatches.load (), 0);
    EXPECT_EQ (automatic_handler_scope_dependency_t::created.load (),
               automatic_handler_scope_dependency_t::destroyed.load ());
}

TEST (ZLinkFrameworkStoreLocationResolvers,
      AppFanoutTypedHandlerUsesPacketNameAcrossTopicsAndPreservesContext)
{
    auto store = std::make_shared<in_memory_location_store_t> ();
    auto app = zlink::framework::app_t::create ();
    fanout_topic_context_handler_t::observed_a.store (false, std::memory_order_release);
    fanout_topic_context_handler_t::observed_b.store (false, std::memory_order_release);
    fanout_topic_context_publish_client_t *client = nullptr;

    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &options) {
        options.add_location_store (store);
        options.handlers ()
          .group ("fanout-topic-context")
          .add_publish<fanout_topic_context_handler_t> ();
        options.add_fanout_channel ("fanout-topic-context")
          .set_routing_id (zlink::routing_id_t::from ("fanout-topic-context-publisher"))
          .enable_publisher ("tcp://127.0.0.1:0")
          .enable_subscriber ()
          .use_handler_group ("fanout-topic-context");
    });
    auto service = std::make_unique<fanout_topic_context_publish_client_t> (app);
    client = service.get ();
    app.add_hosted_service (std::move (service));

    EXPECT_EQ (0, app.run (0, nullptr));
    ASSERT_NE (nullptr, client);
    EXPECT_TRUE (fanout_topic_context_handler_t::observed_a.load (std::memory_order_acquire));
    EXPECT_TRUE (fanout_topic_context_handler_t::observed_b.load (std::memory_order_acquire));
    EXPECT_TRUE (client->last_error.empty ()) << client->last_error;
}

TEST (ZLinkFrameworkStoreLocationResolvers, SameEndpointUsesRidAndOwnerLease)
{
    using namespace zlink::framework;
    using tests::owner_lease_time_store_t;
    for (const bool live : {true, false}) {
        for (const auto &rid : {"remote", "local"}) {
            SCOPED_TRACE (std::string (rid) + (live ? " live" : " expired"));
            in_memory_location_repository_t seed;
            seed.configure_reclaim_margin (std::chrono::milliseconds::zero ());
            seed_mesh_node (seed, "owner", "mesh", rid, "tcp://shared:1");
            auto descriptor = seed.list_mesh_nodes ("mesh", {}).result ().value ().items.front ();
            runtime::in_memory_location_store_t inner;
            runtime::provider_location_repository_t writer (inner);
            writer.configure_reclaim_margin (std::chrono::milliseconds::zero ());
            const auto claim =
              writer.claim_owner_lease ("owner", std::chrono::seconds (30)).result ().value ();
            descriptor.lease_generation =
              std::get<owner_lease_claimed_t> (claim).token.lease_generation;
            ASSERT_EQ (location_write_status_t::stored,
                       writer.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                         .result ()
                         .value ()
                         .status);
            owner_lease_time_store_t observed (inner, "owner",
                                               live
                                                 ? owner_lease_time_store_t::lease_view_t::live
                                                 : owner_lease_time_store_t::lease_view_t::expired);
            runtime::provider_location_repository_t repository (observed);
            repository.configure_reclaim_margin (std::chrono::milliseconds::zero ());
            runtime::live_location_reader_t reader (repository);
            auto sources = reader.poll_mesh_sources ("mesh").result ().value ();
            ASSERT_EQ (1u, sources->size ());
            EXPECT_EQ (live, sources->front ().available ());
            const auto desired =
              runtime::location_auto_connect_test_access_t::desired_rids (sources);
            EXPECT_EQ ((live && std::string (rid) != "local" ? std::vector<std::string>{"remote"}
                                                             : std::vector<std::string>{}),
                       desired);
        }
    }
}
} // namespace
