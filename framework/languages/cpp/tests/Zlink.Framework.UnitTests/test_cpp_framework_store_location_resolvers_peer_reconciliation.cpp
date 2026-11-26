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

bool wait_until (const std::function<bool ()> &predicate)
{
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (predicate ()) {
            return true;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (25));
    }
    return predicate ();
}

TEST (ZLinkFrameworkStoreLocationResolvers, AutoConnectHostReconcilesRouteMeshConnections)
{
    auto store = std::make_shared<in_memory_location_repository_t> ();
    location_options_t options;
    options.owner_lease_renew_interval = std::chrono::milliseconds (50);
    auto runtime = std::make_shared<location_runtime_t> (*store, options, "owner-route-local");
    runtime->start (zlink::routing_id_t::from ("route-z-local-node"));

    seed_mesh_node (*store, "owner-route-remote", "route.mesh", "route-a-remote-node",
                    "inproc://route-remote");
    seed_mesh_node (*store, "owner-route-invalid-dealer", "route.mesh", "route-dealer-peer",
                    "inproc://route-invalid-dealer",
                    zlink::framework::framework_runtime_state_t::stopped);

    zlink::framework::zlink_builder_t zlink = zlink::framework::test::runtime_failure_builder ();
    zlink.route_channel ("route.mesh")
      .set_routing_id (zlink::routing_id_t::from ("route-z-local-node"))
      .connect ("inproc://route-manual");
    auto manager = zlink::framework::detail::channel_runtime_manager_t::from (zlink);
    manager.initialize_route_channels (zlink);
    auto &route = manager.get_route_channel ("route.mesh");

    zlink::framework::service_collection_t services;
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

    EXPECT_TRUE (wait_until ([&] {
        const auto connections = route.list_connections ();
        return std::find (connections.begin (), connections.end (), "inproc://route-remote")
               != connections.end ();
    }));
    auto connected = route.list_connections ();
    EXPECT_NE (connected.end (),
               std::find (connected.begin (), connected.end (), "inproc://route-manual"));
    EXPECT_EQ (connected.end (),
               std::find (connected.begin (), connected.end (), "inproc://route-invalid-dealer"));

    ASSERT_EQ (
      location_write_status_t::stored,
      store
        ->remove_mesh_node ({"route.mesh", zlink::routing_id_t::from ("route-a-remote-node")},
                            live_owner_token (*store, "owner-route-remote"))
        .result ()
        .value ());
    EXPECT_TRUE (wait_until ([&] {
        const auto connections = route.list_connections ();
        return std::find (connections.begin (), connections.end (), "inproc://route-remote")
                 == connections.end ()
               && std::find (connections.begin (), connections.end (), "inproc://route-manual")
                    != connections.end ();
    }));

    service.stop ();
    ASSERT_EQ (
      location_write_status_t::stored,
      store
        ->remove_mesh_node ({"route.mesh", zlink::routing_id_t::from ("route-dealer-peer")},
                            live_owner_token (*store, "owner-route-invalid-dealer"))
        .result ()
        .value ());
    runtime->stop ();
}

TEST (ZLinkFrameworkStoreLocationResolvers, AutoConnectSameEndpointReplacementPrefersNewRid)
{
    auto store = std::make_shared<in_memory_location_repository_t> ();
    location_options_t options;
    options.polling_interval = std::chrono::milliseconds (20);
    options.owner_lease_renew_interval = std::chrono::milliseconds (50);
    auto runtime =
      std::make_shared<location_runtime_t> (*store, options, "owner-route-replacement-local");
    runtime->start (zlink::routing_id_t::from ("route-0-replacement-local"));

    constexpr auto endpoint = "inproc://route-replacement-peer";
    seed_mesh_node (*store, "owner-route-replacement-old", "route.replacement",
                    "route-z-replacement-old", endpoint);
    std::this_thread::sleep_for (std::chrono::milliseconds (5));
    seed_mesh_node (*store, "owner-route-replacement-new", "route.replacement",
                    "route-a-replacement-new", endpoint);
    seed_mesh_node (*store, "owner-route-replacement-same-endpoint", "route.replacement",
                    "route-z-replacement-same-endpoint", "inproc://route-replacement-local");

    zlink::framework::zlink_builder_t zlink = zlink::framework::test::runtime_failure_builder ();
    zlink.route_channel ("route.replacement")
      .bind ("inproc://route-replacement-local")
      .set_routing_id (zlink::routing_id_t::from ("route-0-replacement-local"));
    auto manager = zlink::framework::detail::channel_runtime_manager_t::from (zlink);
    manager.initialize_route_channels (zlink);
    auto &route = manager.get_route_channel ("route.replacement");

    zlink::framework::service_collection_t services;
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

    const auto replacement_selected = wait_until ([&] {
        const auto targets = route.list_connection_targets ();
        const auto has_new =
          std::any_of (targets.begin (), targets.end (), [] (const auto &target) {
              return target.peer_rid && target.peer_rid->to_string () == "route-a-replacement-new";
          });
        const auto has_old =
          std::any_of (targets.begin (), targets.end (), [] (const auto &target) {
              return target.peer_rid && target.peer_rid->to_string () == "route-z-replacement-old";
          });
        const auto has_same_endpoint_peer =
          std::any_of (targets.begin (), targets.end (), [] (const auto &target) {
              return target.peer_rid
                     && target.peer_rid->to_string () == "route-z-replacement-same-endpoint";
          });
        return has_new && !has_old && has_same_endpoint_peer && targets.size () == 2;
    });
    const auto final_targets = route.list_connection_targets ();
    std::string selected_targets;
    for (const auto &target : final_targets) {
        selected_targets +=
          " [" + target.endpoint + "="
          + (target.peer_rid ? target.peer_rid->to_string () : std::string ("none")) + "]";
    }
    ASSERT_TRUE (replacement_selected)
      << "target_count=" << final_targets.size () << " targets=" << selected_targets;

    service.stop ();
    runtime->stop ();
}

TEST (ZLinkFrameworkStoreLocationResolvers,
      AutoConnectRetainsExistingMaintenanceMeshPipeUntilDescriptorRetires)
{
    auto store = std::make_shared<in_memory_location_repository_t> ();
    store->configure_reclaim_margin (std::chrono::milliseconds::zero ());
    location_options_t options;
    options.polling_interval = std::chrono::milliseconds (20);
    options.owner_lease_renew_interval = std::chrono::milliseconds (50);
    auto runtime =
      std::make_shared<location_runtime_t> (*store, options, "owner-route-drain-local");
    runtime->start (zlink::routing_id_t::from ("route-a-drain-local"));
    seed_mesh_node (*store, "owner-route-drain-remote", "route.drain", "route-z-drain-remote",
                    "inproc://route-drain-remote");

    zlink::framework::zlink_builder_t zlink = zlink::framework::test::runtime_failure_builder ();
    zlink.route_channel ("route.drain")
      .bind ("inproc://route-drain-local")
      .set_routing_id (zlink::routing_id_t::from ("route-a-drain-local"));
    auto manager = zlink::framework::detail::channel_runtime_manager_t::from (zlink);
    manager.initialize_route_channels (zlink);
    auto &route = manager.get_route_channel ("route.drain");

    zlink::framework::service_collection_t services;
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
    ASSERT_TRUE (wait_until ([&] {
        const auto connections = route.list_connections ();
        return std::find (connections.begin (), connections.end (), "inproc://route-drain-remote")
               != connections.end ();
    }));

    auto descriptors = store->list_mesh_nodes ("route.drain").result ().value ().items;
    ASSERT_EQ (1u, descriptors.size ());
    auto maintenance = descriptors.front ();
    for (const auto state : {zlink::framework::framework_runtime_state_t::relocating,
                             zlink::framework::framework_runtime_state_t::relocated,
                             zlink::framework::framework_runtime_state_t::draining}) {
        SCOPED_TRACE (static_cast<int> (state));
        ++maintenance.descriptor_revision;
        maintenance.state = state;
        ASSERT_EQ (location_write_status_t::stored,
                   store->update_mesh_node (maintenance, location_write_intent_t::renew)
                     .result ()
                     .value ()
                     .status);

        std::this_thread::sleep_for (std::chrono::milliseconds (100));
        const auto connections = route.list_connections ();
        EXPECT_NE (connections.end (), std::find (connections.begin (), connections.end (),
                                                  "inproc://route-drain-remote"));
    }

    ASSERT_EQ (
      location_write_status_t::stored,
      store
        ->remove_mesh_node ({"route.drain", zlink::routing_id_t::from ("route-z-drain-remote")},
                            live_owner_token (*store, "owner-route-drain-remote"))
        .result ()
        .value ());
    EXPECT_TRUE (wait_until ([&] {
        const auto connections = route.list_connections ();
        return std::find (connections.begin (), connections.end (), "inproc://route-drain-remote")
               == connections.end ();
    }));

    service.stop ();
    runtime->stop ();
}

TEST (ZLinkFrameworkStoreLocationResolvers, AutoConnectHostUsesRouteMeshInitiatorOrdering)
{
    auto store = std::make_shared<in_memory_location_repository_t> ();
    location_options_t options;
    options.owner_lease_renew_interval = std::chrono::milliseconds (50);

    auto lower_runtime =
      std::make_shared<location_runtime_t> (*store, options, "owner-route-lower-local");
    lower_runtime->start (zlink::routing_id_t::from ("route-a-local-node"));
    seed_mesh_node (*store, "owner-route-higher-remote", "route.lower", "route-z-remote-node",
                    "inproc://route-higher-remote");

    zlink::framework::zlink_builder_t lower_zlink =
      zlink::framework::test::runtime_failure_builder ();
    lower_zlink.route_channel ("route.lower")
      .bind ("inproc://route-lower-local")
      .set_routing_id (zlink::routing_id_t::from ("route-a-local-node"));
    auto lower_manager = zlink::framework::detail::channel_runtime_manager_t::from (lower_zlink);
    lower_manager.initialize_route_channels (lower_zlink);
    auto &lower_route = lower_manager.get_route_channel ("route.lower");

    zlink::framework::service_collection_t lower_services;
    lower_services.add_factory<zlink::framework::location_repository_t> (
      [store] (zlink::framework::service_provider_t &) {
          return std::static_pointer_cast<zlink::framework::location_repository_t> (store);
      },
      zlink::framework::service_lifetime_t::singleton);
    lower_services.add_factory<zlink::framework::runtime::live_location_reader_t> (
      [store, options] (zlink::framework::service_provider_t &) {
          return std::make_shared<zlink::framework::runtime::live_location_reader_t> (*store,
                                                                                      options);
      },
      zlink::framework::service_lifetime_t::singleton);
    lower_services.add_factory<location_runtime_t> (
      [lower_runtime] (zlink::framework::service_provider_t &) { return lower_runtime; },
      zlink::framework::service_lifetime_t::singleton);
    auto lower_provider = lower_services.build_provider ();

    zlink::framework::handler_registry_t lower_handlers;
    zlink::framework::serializer_registry_t lower_serializers;
    location_auto_connect_host_service_t lower_service (
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> (),
      lower_zlink.message_bus (),
      zlink::framework::detail::channel_runtime_t::from (lower_zlink.message_bus ())
        .channel_snapshots (),
      lower_handlers, lower_serializers, [&lower_provider] {
          return lower_provider.get_required<zlink::framework::framework_runtime_t> ()
            .status ()
            .state;
      });
    lower_service.start (lower_provider);

    EXPECT_TRUE (wait_until ([&] {
        const auto connections = lower_route.list_connections ();
        const auto targets = lower_route.list_connection_targets ();
        return std::find (connections.begin (), connections.end (), "inproc://route-higher-remote")
                 != connections.end ()
               && std::any_of (targets.begin (), targets.end (), [] (const auto &target) {
                      return target.endpoint == "inproc://route-higher-remote" && target.peer_rid
                             && target.peer_rid->to_string () == "route-z-remote-node";
                  });
    }));
    lower_service.stop ();
    lower_runtime->stop ();

    ASSERT_EQ (
      location_write_status_t::stored,
      store
        ->remove_mesh_node ({"route.lower", zlink::routing_id_t::from ("route-z-remote-node")},
                            live_owner_token (*store, "owner-route-higher-remote"))
        .result ()
        .value ());

    auto higher_runtime =
      std::make_shared<location_runtime_t> (*store, options, "owner-route-higher-local");
    higher_runtime->start (zlink::routing_id_t::from ("route-z-local-node"));
    seed_mesh_node (*store, "owner-route-lower-remote", "route.higher", "route-a-remote-node",
                    "inproc://route-lower-remote");

    zlink::framework::zlink_builder_t higher_zlink =
      zlink::framework::test::runtime_failure_builder ();
    higher_zlink.route_channel ("route.higher")
      .bind ("inproc://route-higher-local")
      .set_routing_id (zlink::routing_id_t::from ("route-z-local-node"));
    auto higher_manager = zlink::framework::detail::channel_runtime_manager_t::from (higher_zlink);
    higher_manager.initialize_route_channels (higher_zlink);
    auto &higher_route = higher_manager.get_route_channel ("route.higher");

    zlink::framework::service_collection_t higher_services;
    higher_services.add_factory<zlink::framework::location_repository_t> (
      [store] (zlink::framework::service_provider_t &) {
          return std::static_pointer_cast<zlink::framework::location_repository_t> (store);
      },
      zlink::framework::service_lifetime_t::singleton);
    higher_services.add_factory<zlink::framework::runtime::live_location_reader_t> (
      [store, options] (zlink::framework::service_provider_t &) {
          return std::make_shared<zlink::framework::runtime::live_location_reader_t> (*store,
                                                                                      options);
      },
      zlink::framework::service_lifetime_t::singleton);
    higher_services.add_factory<location_runtime_t> (
      [higher_runtime] (zlink::framework::service_provider_t &) { return higher_runtime; },
      zlink::framework::service_lifetime_t::singleton);
    auto higher_provider = higher_services.build_provider ();

    zlink::framework::handler_registry_t higher_handlers;
    zlink::framework::serializer_registry_t higher_serializers;
    location_auto_connect_host_service_t higher_service (
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> (),
      higher_zlink.message_bus (),
      zlink::framework::detail::channel_runtime_t::from (higher_zlink.message_bus ())
        .channel_snapshots (),
      higher_handlers, higher_serializers, [&higher_provider] {
          return higher_provider.get_required<zlink::framework::framework_runtime_t> ()
            .status ()
            .state;
      });
    higher_service.start (higher_provider);

    std::this_thread::sleep_for (std::chrono::milliseconds (250));
    const auto higher_connections = higher_route.list_connections ();
    EXPECT_EQ (higher_connections.end (),
               std::find (higher_connections.begin (), higher_connections.end (),
                          "inproc://route-lower-remote"));

    higher_service.stop ();
    higher_runtime->stop ();
    ASSERT_EQ (
      location_write_status_t::stored,
      store
        ->remove_mesh_node ({"route.higher", zlink::routing_id_t::from ("route-a-remote-node")},
                            live_owner_token (*store, "owner-route-lower-remote"))
        .result ()
        .value ());
}
} // namespace
