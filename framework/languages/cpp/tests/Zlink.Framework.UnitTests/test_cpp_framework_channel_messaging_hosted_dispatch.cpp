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
#include "../support/channel_messaging_channel_fixture.hpp"
#include "../support/channel_messaging_handler_fixture.hpp"

using namespace framework_channel_messaging_fixture;

namespace
{
class nested_request_handler_t
{
  public:
    explicit nested_request_handler_t (zlink::framework::channel_client_t &client) :
        _client (client)
    {
    }

    zlink::framework::task_t<reply_t> handle_request (const request_t &request)
    {
        if (request.value == 50) {
            auto nested = co_await _client.request ("hosted-nested", request_t{51})
                            .timeout (std::chrono::milliseconds (2000))
                            .async<reply_t> ();
            co_return reply_t{nested.value + 1};
        }
        co_return reply_t{request.value + 100};
    }

  private:
    zlink::framework::channel_client_t &_client;
};

struct scoped_channel_dependency_t
{
    scoped_channel_dependency_t () { ++created; }
    ~scoped_channel_dependency_t () { ++destroyed; }

    inline static std::atomic_int created{0};
    inline static std::atomic_int destroyed{0};
    int offset = 300;
};

class scoped_channel_filter_t
{
  public:
    explicit scoped_channel_filter_t (scoped_channel_dependency_t &dependency) :
        _dependency (dependency)
    {
    }

    zlink::framework::task_t<void> invoke (const zlink::framework::handler_filter_context_t &,
                                           zlink::framework::handler_next_t next)
    {
        _dependency.offset = 400;
        co_await next ();
        co_return;
    }

  private:
    scoped_channel_dependency_t &_dependency;
};

class scoped_channel_handler_t
{
  public:
    explicit scoped_channel_handler_t (scoped_channel_dependency_t &dependency) :
        _dependency (dependency)
    {
    }

    reply_t handle_request (const request_t &request)
    {
        return {request.value + _dependency.offset};
    }

  private:
    scoped_channel_dependency_t &_dependency;
};
} // namespace

namespace framework_channel_messaging_tests
{
int verify_hosted_dispatch (
  zlink::framework::zlink_builder_t &zlink,
  zlink::framework::detail::channel_runtime_t &outbound_runtime,
  std::vector<zlink::framework::message_dispatch_error_event_t> &dispatch_errors,
  std::mutex &dispatch_errors_mutex,
  zlink::framework::dispatch_options_t &local_dispatch,
  zlink::framework::service_provider_t &provider,
  zlink::framework::serializer_registry_t &serializers,
  zlink::framework::handler_registry_t &handlers,
  zlink::framework::detail::channel_runtime_t &local_runtime,
  zlink::framework::runtime::messaging::envelope_codec_t &envelope_codec,
  zlink::framework::runtime::messaging::envelope_header_t &request_header,
  zlink::framework::runtime::messaging::message_parts_t &request_parts,
  zlink::framework::runtime::messaging::message_parts_t &route_request_parts,
  zlink::framework::detail::channel_packet_dispatcher_t &packet_dispatcher,
  std::vector<zlink::framework::message_dispatch_error_event_t> &observed_dispatch_errors)
{
    const auto framework_core_context = std::make_shared<zlink::context_t> ();
    // The server binds an ephemeral port first; the client connects to the
    // endpoint it resolved.
    zlink::context_t native_bus_server_context;
    zlink::router_socket_t native_bus_server (native_bus_server_context);
    zlink::socket_monitor_t native_bus_server_monitor = native_bus_server.monitor_open ();
    native_bus_server.bind ("tcp://127.0.0.1:*");
    const auto native_bus_endpoint = native_bus_server.options ().last_endpoint ();
    zlink::framework::zlink_builder_t native_bus_builder =
      zlink::framework::test::runtime_failure_builder ();
    native_bus_builder.channel ("native-bus").enable_client ().connect (native_bus_endpoint);
    auto native_bus_runtime =
      zlink::framework::detail::channel_runtime_t::from (native_bus_builder.message_bus ());
    native_bus_runtime.bind_core_context (framework_core_context);
    native_bus_runtime.bind_serializers (serializers);
    auto native_bus_server_done = std::async (std::launch::async, [&] () -> int {
        for (int request_index = 0; request_index < 2; ++request_index) {
            zlink::received_t native_received;
            if (native_bus_server.recv (native_received) != 0 || !native_received.reply_token ()) {
                return 80;
            }
            const auto native_dispatch_reply = packet_dispatcher.dispatch_server_message (
              "local", copy_message_parts (native_received.parts ()), provider, serializers,
              handlers);
            if (!native_dispatch_reply || native_dispatch_reply.value ().size () != 2) {
                return 81;
            }
            zlink::message_t reply_header =
              zlink::message_t::from (native_dispatch_reply.value ()[0].to_string ());
            zlink::message_t reply_body =
              zlink::message_t::from (native_dispatch_reply.value ()[1].to_string ());
            native_received.reply ().message (reply_header).message (reply_body).submit ();
        }
        return 0;
    });
    auto native_bus_reply = native_bus_builder.request_client ("native-bus")
                              .request (request_t{27})
                              .timeout (std::chrono::milliseconds (2000))
                              .async<reply_t> ()
                              .result ();
    const bool native_bus_reply_valid = native_bus_reply && native_bus_reply.value ().value == 127;
    auto native_bus_missing_reply = native_bus_builder.request_client ("native-bus")
                                      .request (missing_probe_request_t{27})
                                      .timeout (std::chrono::milliseconds (2000))
                                      .async<reply_t> ()
                                      .result ();
    const bool native_bus_missing_reply_valid =
      !native_bus_missing_reply
      && native_bus_missing_reply.error_kind ()
           == zlink::framework::framework_error_kind_t::not_found;
    const int native_bus_server_result = native_bus_server_done.get ();
    if (native_bus_server_result != 0) {
        return native_bus_server_result;
    }
    if (!native_bus_reply_valid) {
        return 82;
    }
    if (!native_bus_missing_reply_valid) {
        return 246;
    }

    zlink::framework::runtime::messaging::envelope_header_t validation_header;
    validation_header.kind = zlink::framework::runtime::messaging::message_kind_t::response;
    validation_header.channel_name = "native-bus";
    validation_header.message_name = "request";
    validation_header.topic = "request";
    validation_header.correlation_id = "native-reply-validation";
    auto valid_native_reply = envelope_codec.encode_raw_body_parts (
      validation_header, zlink::message_t::from (std::string ("32")));
    if (!zlink::framework::detail::validate_channel_native_reply (valid_native_reply)) {
        return 242;
    }
    std::vector<zlink::message_t> one_frame_reply;
    one_frame_reply.push_back (zlink::message_t::from (std::string ("bad-header")));
    if (zlink::framework::detail::validate_channel_native_reply (
          zlink::framework::runtime::messaging::message_parts_t (std::move (one_frame_reply)))) {
        return 243;
    }
    std::vector<zlink::message_t> three_frame_reply;
    three_frame_reply.push_back (zlink::message_t::from (valid_native_reply[0].to_string ()));
    three_frame_reply.push_back (zlink::message_t::from (valid_native_reply[1].to_string ()));
    three_frame_reply.push_back (zlink::message_t::from (std::string ("extra-frame")));
    if (zlink::framework::detail::validate_channel_native_reply (
          zlink::framework::runtime::messaging::message_parts_t (std::move (three_frame_reply)))) {
        return 244;
    }
    validation_header.kind = zlink::framework::runtime::messaging::message_kind_t::request;
    auto request_kind_reply = envelope_codec.encode_raw_body_parts (
      validation_header, zlink::message_t::from (std::string ("32")));
    if (zlink::framework::detail::validate_channel_native_reply (request_kind_reply)) {
        return 245;
    }

    {
        // Direct-mode ClientServer listener: wildcard bind, advertised host + bound port.
        zlink::framework::zlink_builder_t advertised_builder =
          zlink::framework::test::runtime_failure_builder ();
        const auto advertised_listeners =
          std::make_shared<zlink::framework::runtime::listener_status_registry_t> ();
        advertised_builder.channel ("hosted-advertised").enable_server ().bind ("tcp://0.0.0.0:0");
        auto advertised_runtime =
          zlink::framework::detail::channel_runtime_t::from (advertised_builder.message_bus ());
        advertised_runtime.bind_core_context (framework_core_context);
        advertised_runtime.bind_serializers (serializers);
        zlink::framework::runtime::channel_host_service_t advertised_service (
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> (),
          advertised_builder.message_bus (), advertised_runtime.channel_snapshots (), handlers,
          serializers, {{"hosted-advertised", "cs.example.internal"}}, {}, advertised_listeners);
        advertised_service.start (provider);
        const auto advertised_endpoint =
          advertised_listeners
            ->find (zlink::framework::listener_kind_t::client_server, "hosted-advertised")
            .value ()
            .endpoint;
        advertised_service.stop ();
        const std::string advertised_prefix = "tcp://cs.example.internal:";
        if (advertised_endpoint.rfind (advertised_prefix, 0) != 0
            || advertised_endpoint.size () == advertised_prefix.size ()
            || advertised_endpoint.substr (advertised_prefix.size ()) == "0") {
            return 414;
        }
    }

    zlink::framework::zlink_builder_t hosted_builder =
      zlink::framework::test::runtime_failure_builder ();
    const auto hosted_listeners =
      std::make_shared<zlink::framework::runtime::listener_status_registry_t> ();
    const auto hosted_server_rid = zlink::routing_id_t::from (std::string ("hosted-server"));
    auto hosted_channel = hosted_builder.channel ("hosted");
    hosted_channel.enable_server ().set_routing_id (hosted_server_rid).bind ("tcp://127.0.0.1:0");
    auto hosted_runtime =
      zlink::framework::detail::channel_runtime_t::from (hosted_builder.message_bus ());
    hosted_runtime.bind_core_context (framework_core_context);
    hosted_runtime.bind_serializers (serializers);
    zlink::framework::runtime::channel_host_service_t hosted_service (
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> (),
      hosted_builder.message_bus (),
      zlink::framework::detail::channel_runtime_t::from (hosted_builder.message_bus ())
        .channel_snapshots (),
      handlers, serializers, {}, {}, hosted_listeners);
    hosted_service.start (provider);
    const auto hosted_endpoint =
      hosted_listeners->find (zlink::framework::listener_kind_t::client_server, "hosted")
        .value ()
        .endpoint;
    hosted_channel.enable_client ().connect (hosted_endpoint);
    auto hosted_reply = hosted_builder.request_client ("hosted")
                          .request (request_t{28})
                          .timeout (std::chrono::milliseconds (2000))
                          .async<reply_t> ()
                          .result ();
    if (!hosted_reply || hosted_reply.value ().value != 128) {
        hosted_service.stop ();
        return 83;
    }
    auto hosted_bus_reply = hosted_builder.message_bus ()
                              .request ("hosted", request_t{29})
                              .timeout (std::chrono::milliseconds (2000))
                              .async<reply_t> ()
                              .result ();
    if (!hosted_bus_reply || hosted_bus_reply.value ().value != 129) {
        hosted_service.stop ();
        return 246;
    }
    hosted_builder.message_bus ().send ("hosted", event_t{30}).async ();
    for (int attempt = 0;
         attempt < 50 && provider.get_required<local_handler_state_t> ().last_event != 30;
         ++attempt) {
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
    if (provider.get_required<local_handler_state_t> ().last_event != 30) {
        hosted_service.stop ();
        return 87;
    }
    auto &hosted_handler = provider.get_required<local_handler_state_t> ();
    hosted_builder.message_bus ().send ("hosted", event_t{300}).async ();
    bool blocking_send_started = false;
    {
        std::unique_lock lock (hosted_handler.send_gate_mutex);
        blocking_send_started = hosted_handler.send_gate_changed.wait_for (
          lock, std::chrono::seconds (1), [&] { return hosted_handler.blocking_send_entered; });
    }
    if (!blocking_send_started) {
        hosted_service.stop ();
        return 247;
    }
    {
        std::lock_guard lock (hosted_handler.send_gate_mutex);
        hosted_handler.release_blocking_send = true;
    }
    hosted_handler.send_gate_changed.notify_all ();
    // The hosted channel listens on an inproc endpoint of the framework context.
    zlink::context_t &peer_context = *framework_core_context;
    zlink::router_socket_t peer_router (peer_context);
    zlink::framework::test::completion_poller_driver_t peer_completion_owner (peer_router);
    peer_router.connect (hosted_endpoint);
    zlink::framework::runtime::messaging::envelope_header_t hosted_header;
    hosted_header.kind = zlink::framework::runtime::messaging::message_kind_t::request;
    hosted_header.channel_name = "hosted";
    hosted_header.message_name = "request";
    hosted_header.topic = "request";
    hosted_header.correlation_id = "hosted-router-request";
    auto hosted_parts = envelope_codec.encode_raw_body_parts (
      hosted_header, zlink::message_t::from (std::string ("29")));
    std::vector<zlink::message_t> routed_hosted_reply;
    bool routed_request_completed = false;
    for (int attempt = 0; attempt < 50 && !routed_request_completed; ++attempt) {
        try {
            auto attempt_header = zlink::message_t::from (hosted_parts[0].to_string ());
            auto attempt_body = zlink::message_t::from (hosted_parts[1].to_string ());
            auto pending = peer_router.request (hosted_server_rid)
                             .message (attempt_header)
                             .message (attempt_body)
                             .timeout (std::chrono::milliseconds (200))
                             .async ()
                             .reply;
            routed_hosted_reply = await_native_reply (std::move (pending)).result ().value ();
            routed_request_completed = true;
        }
        catch (const std::exception &) {
            std::this_thread::sleep_for (std::chrono::milliseconds (20));
        }
    }
    if (routed_hosted_reply.size () != 2) {
        hosted_service.stop ();
        return 85;
    }
    auto routed_reply_parts = copy_message_parts (routed_hosted_reply);
    const auto routed_reply_header = envelope_codec.decode_header (routed_reply_parts);
    const auto routed_reply_body = envelope_codec.decode_body (routed_reply_parts);
    if (!routed_reply_header
        || routed_reply_header.value ().kind
             != zlink::framework::runtime::messaging::message_kind_t::response
        || routed_reply_header.value ().correlation_id != "hosted-router-request"
        || !routed_reply_body
        || serializers.get<reply_t> ()
               .deserialize (
                 zlink::framework::detail::encoded_payload_from_raw (routed_reply_body.value ()))
               .value
             != 129) {
        hosted_service.stop ();
        return 86;
    }
    hosted_service.stop ();

    zlink::framework::zlink_builder_t manual_server_builder =
      zlink::framework::test::runtime_failure_builder ();
    manual_server_builder.channel ("hosted-manual").enable_server ().bind ("tcp://127.0.0.1:0");
    auto manual_server_runtime =
      zlink::framework::detail::channel_runtime_t::from (manual_server_builder.message_bus ());
    manual_server_runtime.bind_core_context (framework_core_context);
    manual_server_runtime.bind_serializers (serializers);
    zlink::framework::runtime::channel_host_service_t manual_hosted_service (
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> (),
      manual_server_builder.message_bus (),
      zlink::framework::detail::channel_runtime_t::from (manual_server_builder.message_bus ())
        .channel_snapshots (),
      handlers, serializers, {}, {}, hosted_listeners);
    manual_hosted_service.start (provider);
    const auto manual_hosted_endpoint =
      hosted_listeners->find (zlink::framework::listener_kind_t::client_server, "hosted-manual")
        .value ()
        .endpoint;

    zlink::framework::zlink_builder_t manual_client_builder =
      zlink::framework::test::runtime_failure_builder ();
    manual_client_builder.channel ("hosted-manual")
      .enable_client ()
      .connect (manual_hosted_endpoint);
    auto manual_client_runtime =
      zlink::framework::detail::channel_runtime_t::from (manual_client_builder.message_bus ());
    manual_client_runtime.bind_core_context (framework_core_context);
    manual_client_runtime.bind_serializers (serializers);

    bool manual_reply_completed = false;
    reply_t manual_reply_value;
    const auto manual_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
    while (std::chrono::steady_clock::now () < manual_deadline && !manual_reply_completed) {
        auto manual_reply = manual_client_builder.request_client ("hosted-manual")
                              .request (request_t{33})
                              .timeout (std::chrono::milliseconds (500))
                              .async<reply_t> ()
                              .result ();
        if (manual_reply) {
            manual_reply_value = manual_reply.value ();
            manual_reply_completed = true;
            break;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (25));
    }
    if (!manual_reply_completed || manual_reply_value.value != 133) {
        manual_hosted_service.stop ();
        return 88;
    }
    auto outage_reply = manual_client_builder.request_client ("hosted-manual")
                          .request (request_t{35})
                          .timeout (std::chrono::milliseconds (1000))
                          .async<reply_t> ()
                          .result ();
    if (!outage_reply || outage_reply.value ().value != 135) {
        manual_hosted_service.stop ();
        return 90;
    }
    manual_hosted_service.stop ();
    const auto stale_start = std::chrono::steady_clock::now ();
    auto stale_reply = manual_client_builder.request_client ("hosted-manual")
                         .request (request_t{34})
                         .timeout (std::chrono::milliseconds (100))
                         .async<reply_t> ()
                         .result ();
    const auto stale_elapsed = std::chrono::steady_clock::now () - stale_start;
    const auto stale_boundary = stale_reply.error () != nullptr
                                  ? zlink::framework::detail::boundary_state (*stale_reply.error ())
                                  : zlink::framework::detail::boundary_error_t::none;
    if (stale_reply
        || (stale_boundary != zlink::framework::detail::boundary_error_t::timed_out
            && stale_boundary != zlink::framework::detail::boundary_error_t::disconnected)
        || stale_elapsed > std::chrono::seconds (2)) {
        return 89;
    }

    zlink::framework::zlink_builder_t nested_hosted_builder =
      zlink::framework::test::runtime_failure_builder ();
    auto nested_hosted_channel = nested_hosted_builder.channel ("hosted-nested");
    nested_hosted_channel.enable_server ().bind ("tcp://127.0.0.1:0");
    auto nested_hosted_runtime =
      zlink::framework::detail::channel_runtime_t::from (nested_hosted_builder.message_bus ());
    nested_hosted_runtime.bind_core_context (framework_core_context);
    nested_hosted_runtime.bind_serializers (serializers);
    zlink::framework::service_collection_t nested_services;
    nested_services.add_singleton<zlink::framework::channel_client_t> (
      std::make_unique<zlink::framework::channel_client_t> (nested_hosted_builder.message_bus ()));
    auto nested_provider = nested_services.build_provider ();
    zlink::framework::handler_registry_t nested_handlers;
    nested_handlers.on_request<nested_request_handler_t, request_t, reply_t> (
      "hosted-nested", "request", &nested_request_handler_t::handle_request,
      {.packet_name = request_t::packet_name});
    zlink::framework::runtime::channel_host_service_t nested_hosted_service (
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> (),
      nested_hosted_builder.message_bus (),
      zlink::framework::detail::channel_runtime_t::from (nested_hosted_builder.message_bus ())
        .channel_snapshots (),
      nested_handlers, serializers, {}, {}, hosted_listeners);
    nested_hosted_service.start (nested_provider);
    const auto nested_hosted_endpoint =
      hosted_listeners->find (zlink::framework::listener_kind_t::client_server, "hosted-nested")
        .value ()
        .endpoint;
    nested_hosted_channel.enable_client ().connect (nested_hosted_endpoint);
    auto nested_hosted_reply = nested_hosted_builder.request_client ("hosted-nested")
                                 .request (request_t{50})
                                 .timeout (std::chrono::milliseconds (2000))
                                 .async<reply_t> ()
                                 .result ();
    nested_hosted_service.stop ();
    if (!nested_hosted_reply || nested_hosted_reply.value ().value != 152) {
        return 84;
    }

    zlink::framework::zlink_builder_t scoped_hosted_builder =
      zlink::framework::test::runtime_failure_builder ();
    auto scoped_hosted_channel = scoped_hosted_builder.channel ("hosted-scoped");
    scoped_hosted_channel.enable_server ().bind ("tcp://127.0.0.1:0");
    auto scoped_hosted_runtime =
      zlink::framework::detail::channel_runtime_t::from (scoped_hosted_builder.message_bus ());
    scoped_hosted_runtime.bind_core_context (framework_core_context);
    scoped_hosted_runtime.bind_serializers (serializers);
    zlink::framework::service_collection_t scoped_services;
    scoped_services.add_scoped<scoped_channel_dependency_t> ();
    auto scoped_provider = scoped_services.build_provider ();
    zlink::framework::handler_registry_t scoped_handlers;
    scoped_handlers.use_filter<scoped_channel_filter_t> ();
    scoped_handlers.on_request<scoped_channel_handler_t, request_t, reply_t> (
      "hosted-scoped", "request", &scoped_channel_handler_t::handle_request,
      {.packet_name = request_t::packet_name});
    zlink::framework::runtime::channel_host_service_t scoped_hosted_service (
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> (),
      scoped_hosted_builder.message_bus (),
      zlink::framework::detail::channel_runtime_t::from (scoped_hosted_builder.message_bus ())
        .channel_snapshots (),
      scoped_handlers, serializers, {}, {}, hosted_listeners);
    scoped_hosted_service.start (scoped_provider);
    const auto scoped_hosted_endpoint =
      hosted_listeners->find (zlink::framework::listener_kind_t::client_server, "hosted-scoped")
        .value ()
        .endpoint;
    scoped_hosted_channel.enable_client ().connect (scoped_hosted_endpoint);
    auto scoped_hosted_reply = scoped_hosted_builder.request_client ("hosted-scoped")
                                 .request (request_t{40})
                                 .timeout (std::chrono::milliseconds (2000))
                                 .async<reply_t> ()
                                 .result ();
    auto second_scoped_reply = scoped_hosted_builder.request_client ("hosted-scoped")
                                 .request (request_t{41})
                                 .timeout (std::chrono::milliseconds (2000))
                                 .async<reply_t> ()
                                 .result ();
    scoped_hosted_service.stop ();
    if (!scoped_hosted_reply || scoped_hosted_reply.value ().value != 440 || !second_scoped_reply
        || second_scoped_reply.value ().value != 441
        || scoped_channel_dependency_t::created.load () != 2
        || scoped_channel_dependency_t::destroyed.load () != 2) {
        return 85;
    }

    return verify_channel_receive (
      zlink, outbound_runtime, dispatch_errors, dispatch_errors_mutex, local_dispatch, provider,
      serializers, handlers, local_runtime, envelope_codec, request_header, request_parts,
      route_request_parts, packet_dispatcher, observed_dispatch_errors);
}
} // namespace framework_channel_messaging_tests
