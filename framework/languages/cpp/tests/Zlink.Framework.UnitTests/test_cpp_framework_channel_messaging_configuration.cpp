/* SPDX-License-Identifier: Elastic-2.0 */
#include <zlink/json.hpp>
#include "../support/runtime_failure_fixture.hpp"
#include "../support/telemetry_observation.hpp"
#include <zlink/framework.hpp>
#include <zlink/codecs/protobuf.hpp>
#include "test_completion_poller_driver.hpp"
#include "runtime/channels/channel_packet_dispatcher.hpp"
#include "runtime/configuration/endpoint_connections.hpp"
#include "runtime/locations/spot_address_resolvers.hpp"
#include "runtime/channels/channel_host_service.hpp"
#include "runtime/channels/channel_reply_writer.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/channels/channel_runtime_bundle.hpp"
#include "runtime/channels/channel_runtime_manager.hpp"
#include "runtime/channels/route_channel_runtime.hpp"
#include "runtime/channels/route_channel_registration.hpp"
#include "runtime/channels/route_connection_set.hpp"
#include "runtime/channels/route_handler_registry.hpp"
#include "runtime/channels/route_internal_packet_dispatcher.hpp"
#include "runtime/channels/route_packet_dispatcher.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/diagnostics/listener_status_registry.hpp"
#include "runtime/messaging/client_call_codec.hpp"
#include "runtime/mesh/mesh_record_dispatcher.hpp"
#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/spots/spot_route_internal_dispatcher.hpp"
#include "runtime/spots/spot_route_packets.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include <zlink/Contracts/Core/context.hpp>
#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Eventing/events.hpp>
#include <zlink/Contracts/Eventing/monitor.hpp>
#include <zlink/Contracts/Eventing/poll_event.hpp>
#include <zlink/Contracts/Eventing/poller.hpp>
#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include <zlink/Contracts/Messaging/received.hpp>
#include <zlink/Contracts/Sockets/message_socket_contracts.hpp>
#include <zlink/Contracts/Sockets/results.hpp>
#include <zlink/Contracts/Sockets/routed_socket_contracts.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <vector>
#include "../support/framework_channel_messaging_cases.hpp"

#include "../support/channel_messaging_messages.hpp"

using namespace framework_channel_messaging_fixture;

namespace framework_channel_messaging_tests
{
int verify_channel_configuration ()
{
    zlink::framework::zlink_builder_t zlink = zlink::framework::test::runtime_failure_builder ();
    zlink.add_node ("outbound-node");
    zlink.default_request_timeout (std::chrono::milliseconds (10000));
    zlink.channel ("profile")
      .default_request_timeout (std::chrono::milliseconds (1500))
      .enable_client ()
      .connect ("tcp://127.0.0.1:7101");
    zlink.channel ("events").enable_publisher ().bind ("tcp://127.0.0.1:7201");

    const auto channels =
      zlink::framework::detail::channel_runtime_t::from (zlink.message_bus ()).channel_snapshots ();
    if (channels.size () != 2) {
        return 1;
    }

    zlink::framework::channel_builder_t standalone_channel;
    auto standalone_server = standalone_channel.enable_server ()
                               .bind ("tcp://127.0.0.1:7001")
                               .set_routing_id (zlink::routing_id_t::from ("standalone-server"))
                               .max_message_size (zlink::byte_size_t::bytes (4096))
                               .peer_weight (zlink::peer_weight_t::value (3))
                               .snapshot ();
    standalone_channel.enable_client ().connect ("tcp://127.0.0.1:7002");
    standalone_channel.enable_publisher ().bind ("tcp://127.0.0.1:7003");
    standalone_channel.enable_subscriber ().connect ("tcp://127.0.0.1:7004");
    standalone_channel.default_request_timeout (std::chrono::milliseconds (250));
    const auto standalone_snapshot = standalone_channel.snapshot ();
    if (!standalone_server.enabled || standalone_server.bind_endpoints.size () != 1
        || !standalone_server.routing_id
        || standalone_server.routing_id->to_string () != "standalone-server"
        || !standalone_server.max_message_size
        || standalone_server.max_message_size->bytes () != 4096 || !standalone_server.peer_weight
        || standalone_server.peer_weight->value () != 3 || !standalone_snapshot.client.enabled
        || !standalone_snapshot.publisher.enabled || !standalone_snapshot.subscriber.enabled
        || standalone_snapshot.default_request_timeout != std::chrono::milliseconds (250)) {
        return 130;
    }
    bool invalid_channel_timeout_failed = false;
    try {
        standalone_channel.default_request_timeout (std::chrono::milliseconds (0));
    }
    catch (const zlink::framework::framework_exception_t &error) {
        invalid_channel_timeout_failed =
          error.kind () == zlink::framework::framework_error_kind_t::protocol_error;
    }
    if (!invalid_channel_timeout_failed) {
        return 131;
    }
    zlink::framework::capability_builder_t loose_capability;
    loose_capability.bind ("tcp://127.0.0.1:7051")
      .connect ("tcp://127.0.0.1:7052")
      .set_routing_id (zlink::routing_id_t::from ("loose-capability"))
      .max_message_size (zlink::byte_size_t::bytes (2048))
      .peer_weight (zlink::peer_weight_t::value (6));
    zlink::framework::capability_builder_t moved_capability (std::move (loose_capability));
    zlink::framework::capability_builder_t assigned_capability;
    assigned_capability = std::move (moved_capability);
    const auto assigned_capability_snapshot = assigned_capability.snapshot ();
    if (!assigned_capability_snapshot.enabled
        || assigned_capability_snapshot.bind_endpoints.size () != 1
        || assigned_capability_snapshot.connect_endpoints.size () != 1
        || !assigned_capability_snapshot.routing_id
        || assigned_capability_snapshot.routing_id->to_string () != "loose-capability"
        || !assigned_capability_snapshot.peer_weight
        || assigned_capability_snapshot.peer_weight->value () != 6) {
        return 126;
    }
    zlink::framework::channel_builder_t movable_channel;
    movable_channel.enable_client ().connect ("tcp://127.0.0.1:7054");
    zlink::framework::channel_builder_t moved_channel (std::move (movable_channel));
    zlink::framework::channel_builder_t assigned_channel;
    assigned_channel = std::move (moved_channel);
    if (!assigned_channel.snapshot ().client.enabled) {
        return 124;
    }
    zlink::framework::zlink_builder_t movable_builder =
      zlink::framework::test::runtime_failure_builder ();
    movable_builder.add_node ("movable");
    zlink::framework::zlink_builder_t moved_builder (std::move (movable_builder));
    zlink::framework::zlink_builder_t assigned_builder =
      zlink::framework::test::runtime_failure_builder ();
    assigned_builder = std::move (moved_builder);
    assigned_builder.channel ("moved-channel").enable_client ().connect ("tcp://127.0.0.1:7053");
    const auto assigned_channels =
      zlink::framework::detail::channel_runtime_t::from (assigned_builder.message_bus ())
        .channel_snapshots ();
    if (assigned_channels.size () != 1 || assigned_channels[0].name != "moved-channel") {
        return 125;
    }
    zlink::framework::message_bus_t default_bus;
    zlink::framework::message_bus_t moved_bus (std::move (default_bus));
    zlink::framework::message_bus_t assigned_bus;
    assigned_bus = std::move (moved_bus);
    auto assigned_bus_runtime = zlink::framework::detail::channel_runtime_t::from (assigned_bus);
    if (assigned_bus_runtime.pending_count () != 0
        || assigned_bus.default_request_timeout ("missing-channel")
             <= std::chrono::milliseconds::zero ()
        || assigned_bus_runtime.server_peer_weight_override ("missing-channel")) {
        return 128;
    }
    zlink::framework::route_client_t default_route_client;
    /* Common spec 04-async-execution-policy.ko.md §1.3 (and its terminal table at the top
     * of §1) makes the one-way submit terminator complete exceptionally on failure rather
     * than throw out of submit(); the C++ terminal is task_t<void>, so the admission
     * failure is observed on the returned task. */
    const auto default_route_send =
      default_route_client
        .send_to_node ("missing.route", zlink::routing_id_t::from ("missing-node"), request_t{1})
        .metadata ("trace", "default")
        .async ()
        .result ();
    if (default_route_send
        || default_route_send.error_kind ()
             != zlink::framework::framework_error_kind_t::protocol_error) {
        return 411;
    }
    auto default_route_request =
      default_route_client
        .request_to_node ("missing.route", zlink::routing_id_t::from ("missing-node"), request_t{2})
        .metadata ("trace", "default")
        .timeout (std::chrono::milliseconds (25))
        .async<reply_t> ()
        .result ();
    auto default_spot_route_request =
      default_route_client.request_to_spot ("missing-spot", request_t{3})
        .async<reply_t> ()
        .result ();
    if (default_route_request
        || default_route_request.error_kind ()
             != zlink::framework::framework_error_kind_t::protocol_error
        || default_spot_route_request
        || default_spot_route_request.error_kind ()
             != zlink::framework::framework_error_kind_t::not_configured) {
        return 127;
    }
    bool invalid_builder_timeout_failed = false;
    try {
        zlink.default_request_timeout (std::chrono::milliseconds (0));
    }
    catch (const zlink::framework::framework_exception_t &error) {
        invalid_builder_timeout_failed =
          error.kind () == zlink::framework::framework_error_kind_t::protocol_error;
    }
    bool empty_route_channel_failed = false;
    try {
        (void) zlink.route_channel ("");
    }
    catch (const zlink::framework::framework_exception_t &error) {
        empty_route_channel_failed =
          error.kind () == zlink::framework::framework_error_kind_t::protocol_error;
    }
    auto route_builder = zlink.route_channel ("runtime.route");
    route_builder.bind ("tcp://127.0.0.1:7401")
      .set_routing_id (zlink::routing_id_t::from ("runtime-route"))
      .connect ("tcp://127.0.0.1:7402")
      .default_request_timeout (std::chrono::milliseconds (275))
      .add_handler_group ("runtime-group");
    zlink::framework::route_channel_builder_t moved_route_builder (std::move (route_builder));
    zlink::framework::route_channel_builder_t assigned_route_builder;
    assigned_route_builder = std::move (moved_route_builder);
    assigned_route_builder.add_handler_group ("runtime-group-2");
    if (!invalid_builder_timeout_failed || !empty_route_channel_failed
        || zlink::framework::detail::channel_runtime_manager_t::configured_route_channel_ids (zlink)
             .empty ()) {
        return 129;
    }

    zlink.channel ("runtime-options").enable_server ().bind ("tcp://127.0.0.1:7301");
    zlink::framework::channel_runtime_options_t runtime_options (zlink.message_bus ());
    auto client_server_options = runtime_options.client_server_channel ("runtime-options");
    auto server_socket_options = client_server_options.configure_server_socket ();
    server_socket_options.peer_weight (zlink::peer_weight_t::value (7));
    zlink::framework::channel_server_socket_runtime_options_t moved_server_socket_options (
      std::move (server_socket_options));
    zlink::framework::channel_server_socket_runtime_options_t assigned_server_socket_options;
    assigned_server_socket_options = std::move (moved_server_socket_options);
    assigned_server_socket_options.peer_weight (zlink::peer_weight_t::value (8));
    auto runtime_options_weight =
      zlink::framework::detail::channel_runtime_t::from (zlink.message_bus ())
        .server_peer_weight_override ("runtime-options");
    if (!runtime_options_weight || *runtime_options_weight != 8) {
        return 132;
    }

    auto client = zlink.request_client ("profile");
    auto outbound_runtime =
      zlink::framework::detail::channel_runtime_t::from (zlink.message_bus ());
    auto request_call = client.request (request_t{1})
                          .metadata ("trace-id", "request-trace")
                          .timeout (std::chrono::milliseconds (3000));
    auto request_result = request_call.async<reply_t> ().result ();
    if (request_result
        || (request_result.error () != nullptr
            && zlink::framework::detail::boundary_state (*request_result.error ())
                 != zlink::framework::detail::boundary_error_t::timed_out)) {
        return 2;
    }

    auto default_timeout_result = client.request (request_t{10}).async<reply_t> ().result ();
    if (default_timeout_result
        || (default_timeout_result.error () != nullptr
            && zlink::framework::detail::boundary_state (*default_timeout_result.error ())
                 != zlink::framework::detail::boundary_error_t::timed_out)) {
        return 35;
    }

    auto bus = zlink.message_bus ();
    auto send_call = bus.send ("profile", request_t{2}).metadata ("trace-id", "send-trace");
    send_call.async ().result ().value ();

    zlink.publisher ()
      .publish ("events", "profile.changed", event_t{3})
      .async ()
      .result ()
      .value ();

    try {
        bus.send ("missing", request_t{4}).async ().result ().value ();
    }
    catch (const zlink::framework::framework_exception_t &) {
        // A send to an unknown channel is rejected synchronously by the one-way
        // contract; this probe only checks that the runtime stays alive.
    }

    zlink::framework::zlink_builder_t unbounded_pending =
      zlink::framework::test::runtime_failure_builder ();
    unbounded_pending.channel ("profile").enable_client ();
    auto pending_runtime =
      zlink::framework::detail::channel_runtime_t::from (unbounded_pending.message_bus ());
    const auto first_pending = pending_runtime.reserve_outbound_request ("profile");
    const auto second_pending = pending_runtime.reserve_outbound_request ("profile");
    if (!first_pending || !second_pending || pending_runtime.pending_count () != 2) {
        return 6;
    }
    pending_runtime.drain ();

    zlink::framework::zlink_builder_t outbound_only =
      zlink::framework::test::runtime_failure_builder ();
    auto outbound_only_channel = outbound_only.channel ("client-only");
    outbound_only_channel.enable_client ();
    outbound_only_channel.enable_publisher ();
    const auto outbound_channels =
      zlink::framework::detail::channel_runtime_t::from (outbound_only.message_bus ())
        .channel_snapshots ();
    if (outbound_channels.size () != 1 || outbound_channels[0].server.enabled
        || !outbound_channels[0].client.enabled || !outbound_channels[0].publisher.enabled) {
        return 7;
    }
    zlink::framework::zlink_builder_t fanout = zlink::framework::test::runtime_failure_builder ();
    auto fanout_channel = fanout.channel ("broadcast");
    fanout_channel.enable_publisher ().bind ("tcp://127.0.0.1:7351");
    fanout_channel.enable_subscriber ()
      .connect ("tcp://127.0.0.1:7351")
      .connect ("tcp://127.0.0.1:7352");
    auto fanout_manager = zlink::framework::detail::channel_runtime_manager_t::from (fanout);
    fanout_manager.initialize_publisher_channels ();
    fanout_manager.initialize_inbound_channels ();
    auto &fanout_publisher = fanout_manager.get_or_create_publisher_bundle ("broadcast");
    if (!fanout_publisher.contains_manual_connection ("tcp://127.0.0.1:7351")
        || fanout_manager.monitoring_source ("broadcast.publisher") != "broadcast.publisher"
        || fanout_manager.monitoring_source ("broadcast.subscriber") != "broadcast.subscriber") {
        return 74;
    }

    fanout.channel ("profile")
      .enable_server ()
      .set_routing_id (zlink::routing_id_t::from (std::string ("profile-api")))
      .bind ("tcp://127.0.0.1:7399");
    std::optional<zlink::framework::channel_snapshot_t> routed_server_snapshot;
    for (const auto &channel :
         zlink::framework::detail::channel_runtime_t::from (fanout.message_bus ())
           .channel_snapshots ()) {
        if (channel.name == "profile") {
            routed_server_snapshot = channel;
            break;
        }
    }
    if (!routed_server_snapshot || !routed_server_snapshot->server.routing_id
        || routed_server_snapshot->server.routing_id->to_string () != "profile-api") {
        return 75;
    }

    return verify_dispatch_observer_setup (zlink, outbound_only, outbound_runtime);
}
} // namespace framework_channel_messaging_tests
