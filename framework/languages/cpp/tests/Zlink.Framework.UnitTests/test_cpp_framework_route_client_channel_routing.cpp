/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#include "../support/runtime_failure_fixture.hpp"

#include <zlink/framework.hpp>

#include "runtime/channels/channel_runtime.hpp"
#include "runtime/messaging/envelope_codec.hpp"

#include <zlink/Contracts/Core/context.hpp>
#include <zlink/Contracts/Eventing/monitor.hpp>
#include <zlink/Contracts/Eventing/poller.hpp>
#include <zlink/Contracts/Messaging/topic_message.hpp>
#include <zlink/Contracts/Sockets/pubsub_socket_contracts.hpp>
#include <zlink/Contracts/Sockets/results.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{

struct route_request_t
{
    static constexpr const char *packet_name = "channel-route.request";
    int value{};
};

struct route_reply_t
{
    int value{};
};

struct route_event_t
{
    static constexpr const char *packet_name = "channel-route.event";
    int value{};
};

template <typename T>
void add_int_serializer (zlink::framework::serializer_registry_t &serializers,
                         std::string content_type)
{
    serializers.add<T> (
      [] (const T &value) {
          return zlink::framework::encoded_payload_t::from_string (std::to_string (value.value));
      },
      [] (const zlink::framework::encoded_payload_t &payload) {
          return T{std::stoi (payload.to_string ())};
      },
      std::move (content_type));
}

template <typename T> std::string error_message (const zlink::framework::result_t<T> &result)
{
    return result.error () == nullptr ? "no error detail" : result.error ()->what ();
}

zlink::framework::result_t<void> internal_failure (std::string message)
{
    return zlink::framework::result_t<void>::failure (
      zlink::framework::framework_error_kind_t::internal_failure, std::move (message));
}

} // namespace

TEST (CppFrameworkRouteClientChannelRouting, OneWaySendDoesNotCreateCorrelation)
{
    using zlink::framework::runtime::messaging::envelope_codec_t;
    using zlink::framework::runtime::messaging::message_parts_t;

    zlink::framework::serializer_registry_t serializers;
    add_int_serializer<route_event_t> (serializers, "application/x-route-event");
    zlink::framework::zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    auto runtime = zlink::framework::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    int received = 0;
    runtime.bind_mesh_channel_transport (
      "one-way-route",
      [&] (message_parts_t parts) -> zlink::framework::task_t<zlink::framework::result_t<void>> {
          const auto header = envelope_codec_t{}.decode_header (parts, false);
          EXPECT_TRUE (header);
          if (header) {
              EXPECT_TRUE (header.value ().correlation_id.empty ());
              EXPECT_EQ (header.value ().kind,
                         zlink::framework::runtime::messaging::message_kind_t::command);
              ++received;
          }
          co_return zlink::framework::result_t<void>::success ();
      },
      {});
    const auto sent = builder.route_client (serializers)
                        .send_to_channel ("one-way-route", route_event_t{17})
                        .async ()
                        .result ();
    ASSERT_TRUE (sent) << error_message (sent);
    EXPECT_EQ (received, 1);
}

TEST (CppFrameworkRouteClientChannelRouting, OneWayPublishDoesNotCreateCorrelation)
{
    using namespace std::chrono_literals;
    using zlink::framework::runtime::messaging::envelope_codec_t;
    using zlink::framework::runtime::messaging::message_parts_t;

    const std::string endpoint = "inproc://impl-1207-one-way-publish";
    const std::string topic = "one-way-event";
    auto context = std::make_shared<zlink::context_t> ();
    zlink::sub_socket_t subscriber (*context);
    subscriber.options ().linger (0ms);
    subscriber.options ().recv_timeout (5s);
    auto monitor = subscriber.monitor_open (zlink::monitor_event::connection_ready);
    subscriber.set_subscription (topic);
    subscriber.bind (endpoint);

    zlink::framework::serializer_registry_t serializers;
    add_int_serializer<route_event_t> (serializers, "application/x-route-event");
    zlink::framework::zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    builder.channel ("one-way-publisher").enable_publisher ().connect (endpoint);
    auto runtime = zlink::framework::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_core_context (context);
    runtime.bind_serializers (serializers);
    runtime.initialize_manual_channel_publishers ();
    zlink::poller_t poller;
    poller.add (monitor, zlink::poll_event_flag_t::pollin, 1);
    zlink::poll_event_t ready;
    ASSERT_EQ (poller.wait (&ready, 1, 5s), 1);
    const auto connected = monitor.recv (zlink::recv_flags_t::dontwait);
    ASSERT_TRUE (connected);
    ASSERT_EQ (connected->event, zlink::monitor_event::connection_ready);
    ASSERT_TRUE (monitor.status ().is_ready ());

    const auto published = builder.publisher ()
                             .publish ("one-way-publisher", topic, route_event_t{23})
                             .async ()
                             .result ();
    ASSERT_TRUE (published) << error_message (published);
    zlink::topic_message_t received;
    ASSERT_EQ (subscriber.subscribe (received), static_cast<int> (zlink::recv_result_t::ok));
    EXPECT_EQ (received.topic (), topic);
    std::vector<zlink::message_t> parts;
    for (const auto &part : received.parts ()) {
        parts.push_back (zlink::message_t::from (part.to_string ()));
    }
    const auto header =
      envelope_codec_t{}.decode_header (message_parts_t (std::move (parts)), false);
    ASSERT_TRUE (header);
    EXPECT_TRUE (header.value ().correlation_id.empty ());
    EXPECT_EQ (header.value ().kind, zlink::framework::runtime::messaging::message_kind_t::publish);
}

TEST (CppFrameworkRouteClientChannelRouting, ResolvesRouteMeshAndClientServerChannels)
{
    using namespace std::chrono_literals;
    using zlink::framework::framework_error_kind_t;
    using zlink::framework::runtime::messaging::envelope_codec_t;
    using zlink::framework::runtime::messaging::envelope_header_t;
    using zlink::framework::runtime::messaging::message_kind_t;
    using zlink::framework::runtime::messaging::message_parts_t;

    zlink::framework::serializer_registry_t serializers;
    add_int_serializer<route_request_t> (serializers, "application/x-route-request");
    add_int_serializer<route_reply_t> (serializers, "application/x-route-reply");
    add_int_serializer<route_event_t> (serializers, "application/x-route-event");

    zlink::framework::zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    auto runtime = zlink::framework::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);

    envelope_codec_t envelope;
    std::string route_mesh_send_error;
    std::string route_mesh_request_error;
    std::string client_server_send_error;
    std::string client_server_request_error;
    int route_mesh_send_count = 0;
    int route_mesh_request_count = 0;
    int client_server_send_count = 0;
    int client_server_request_count = 0;

    runtime.bind_mesh_channel_transport (
      "route-mesh-channel",
      [&] (message_parts_t parts) -> zlink::framework::task_t<zlink::framework::result_t<void>> {
          const auto header = envelope.decode_header (parts, false);
          const auto body = envelope.decode_body (parts);
          if (!header || !body || header.value ().kind != message_kind_t::command
              || header.value ().channel_name != "route-mesh-channel"
              || header.value ().message_name != route_event_t::packet_name
              || serializers.get<route_event_t> ()
                     .deserialize (
                       zlink::framework::detail::encoded_payload_from_raw (body.value ()))
                     .value
                   != 17) {
              route_mesh_send_error = "RouteMesh send did not receive the encoded ChannelName call";
              co_return internal_failure (route_mesh_send_error);
          }
          ++route_mesh_send_count;
          co_return zlink::framework::result_t<void>::success ();
      },
      [&] (message_parts_t parts, std::chrono::milliseconds timeout)
        -> zlink::framework::task_t<zlink::framework::result_t<message_parts_t>> {
          const auto header = envelope.decode_header (parts, false);
          const auto body = envelope.decode_body (parts);
          if (!header || !body || header.value ().kind != message_kind_t::request
              || header.value ().channel_name != "route-mesh-channel"
              || header.value ().message_name != route_request_t::packet_name || timeout != 37ms
              || serializers.get<route_request_t> ()
                     .deserialize (
                       zlink::framework::detail::encoded_payload_from_raw (body.value ()))
                     .value
                   != 23) {
              route_mesh_request_error =
                "RouteMesh request did not receive the encoded ChannelName call";
              co_return zlink::framework::result_t<message_parts_t>::failure (
                framework_error_kind_t::internal_failure, route_mesh_request_error);
          }
          EXPECT_FALSE (header.value ().correlation_id.empty ());
          ++route_mesh_request_count;
          auto reply_header = header.value ();
          reply_header.kind = message_kind_t::response;
          reply_header.message_name = "channel-route.reply";
          const auto serialized =
            serializers.get<route_reply_t> ().serialize_with_content_type (route_reply_t{46});
          reply_header.content_type = serialized.content_type;
          auto reply_parts = envelope.encode_raw_body_parts (
            reply_header, zlink::framework::detail::encoded_payload_to_raw (serialized.payload));
          co_return zlink::framework::result_t<message_parts_t>::success (std::move (reply_parts));
      });

    runtime.bind_client_server_transport (
      "client-server-channel",
      [&] (std::string packet_name, std::string content_type, zlink::message_t payload,
           std::map<std::string, std::string> metadata) {
          const auto decoded = serializers.get<route_event_t> ().deserialize (
            zlink::framework::detail::encoded_payload_from_raw (payload));
          if (packet_name != route_event_t::packet_name
              || content_type != "application/x-route-event" || decoded.value != 31
              || metadata != std::map<std::string, std::string>{{"tenant-id", "tenant-42"}}) {
              client_server_send_error = "ClientServer send did not receive the typed payload";
              return zlink::framework::task_t<void> (internal_failure (client_server_send_error));
          }
          ++client_server_send_count;
          return zlink::framework::task_t<void> (zlink::framework::result_t<void>::success ());
      },
      [&] (std::string packet_name, std::string content_type, zlink::message_t payload,
           std::chrono::milliseconds timeout, std::map<std::string, std::string> metadata) {
          const auto decoded = serializers.get<route_request_t> ().deserialize (
            zlink::framework::detail::encoded_payload_from_raw (payload));
          if (packet_name != route_request_t::packet_name
              || content_type != "application/x-route-request" || decoded.value != 19
              || timeout != 41ms
              || metadata != std::map<std::string, std::string>{{"tenant-id", "tenant-42"}}) {
              client_server_request_error =
                "ClientServer request did not receive the typed payload";
              return zlink::framework::task_t<zlink::message_t> (
                zlink::framework::result_t<zlink::message_t>::failure (
                  framework_error_kind_t::internal_failure, client_server_request_error));
          }
          ++client_server_request_count;
          const auto serialized =
            serializers.get<route_reply_t> ().serialize_with_content_type (route_reply_t{38});
          return zlink::framework::task_t<zlink::message_t> (
            zlink::framework::result_t<zlink::message_t>::success (
              zlink::framework::detail::encoded_payload_to_raw (serialized.payload)));
      });

    auto route_client = builder.route_client (serializers);
    const auto route_mesh_send =
      route_client.send_to_channel ("route-mesh-channel", route_event_t{17}).async ().result ();
    ASSERT_TRUE (route_mesh_send) << error_message (route_mesh_send);
    const auto route_mesh_reply =
      route_client.request_to_channel ("route-mesh-channel", route_request_t{23})
        .timeout (37ms)
        .async<route_reply_t> ()
        .result ();
    ASSERT_TRUE (route_mesh_reply) << error_message (route_mesh_reply);
    EXPECT_EQ (46, route_mesh_reply.value ().value);

    const auto client_server_send =
      route_client.send_to_channel ("client-server-channel", route_event_t{31})
        .metadata ("tenant-id", "tenant-42")
        .async ()
        .result ();
    EXPECT_TRUE (client_server_send) << error_message (client_server_send);
    const auto client_server_reply =
      route_client.request_to_channel ("client-server-channel", route_request_t{19})
        .metadata ("tenant-id", "tenant-42")
        .timeout (41ms)
        .async<route_reply_t> ()
        .result ();
    EXPECT_TRUE (client_server_reply) << error_message (client_server_reply);
    if (client_server_reply) {
        EXPECT_EQ (38, client_server_reply.value ().value);
    }

    EXPECT_TRUE (route_mesh_send_error.empty ()) << route_mesh_send_error;
    EXPECT_TRUE (route_mesh_request_error.empty ()) << route_mesh_request_error;
    EXPECT_TRUE (client_server_send_error.empty ()) << client_server_send_error;
    EXPECT_TRUE (client_server_request_error.empty ()) << client_server_request_error;
    EXPECT_EQ (1, route_mesh_send_count);
    EXPECT_EQ (1, route_mesh_request_count);
    EXPECT_EQ (1, client_server_send_count);
    EXPECT_EQ (1, client_server_request_count);

    const auto missing_send =
      route_client.send_to_channel ("unregistered-channel", route_event_t{1}).async ().result ();
    EXPECT_FALSE (missing_send);
    EXPECT_EQ (framework_error_kind_t::not_found, missing_send.error_kind ());
    EXPECT_EQ ("ChannelName 'unregistered-channel' is not registered",
               error_message (missing_send));
    const auto missing_request =
      route_client.request_to_channel ("unregistered-channel", route_request_t{1})
        .async<route_reply_t> ()
        .result ();
    EXPECT_FALSE (missing_request);
    EXPECT_EQ (framework_error_kind_t::not_found, missing_request.error_kind ());
    EXPECT_EQ ("ChannelName 'unregistered-channel' is not registered",
               error_message (missing_request));
}
