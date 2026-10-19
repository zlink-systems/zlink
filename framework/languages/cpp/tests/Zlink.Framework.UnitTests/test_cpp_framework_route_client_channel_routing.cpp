/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "../support/runtime_failure_fixture.hpp"

#include <zlink/framework.hpp>

#include "runtime/channels/channel_runtime.hpp"
#include "runtime/channels/channel_runtime_manager.hpp"
#include "runtime/messaging/envelope_codec.hpp"

#include <zlink/Contracts/Core/context.hpp>
#include <zlink/Contracts/Eventing/monitor.hpp>
#include <zlink/Contracts/Eventing/poller.hpp>
#include <zlink/Contracts/Messaging/topic_message.hpp>
#include <zlink/Contracts/Sockets/pubsub_socket_contracts.hpp>
#include <zlink/Contracts/Sockets/results.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
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

struct transport_copy_counter_t
{
    explicit transport_copy_counter_t (unsigned &copies) : copies (&copies) {}
    transport_copy_counter_t (const transport_copy_counter_t &other) : copies (other.copies)
    {
        ++*copies;
    }
    transport_copy_counter_t (transport_copy_counter_t &&) noexcept = default;
    unsigned *copies;
};

template <typename Predicate> bool wait_until (Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
    while (!predicate ()) {
        if (std::chrono::steady_clock::now () >= deadline)
            return false;
        std::this_thread::yield ();
    }
    return true;
}

} // namespace

TEST (CppFrameworkRouteClientChannelRouting, NodeOperationsRetainTransportWithoutCopyingCallable)
{
    using namespace zlink::framework;
    using runtime::messaging::message_parts_t;
    serializer_registry_t serializers;
    add_int_serializer<route_event_t> (serializers, "application/x-route-event");
    add_int_serializer<route_request_t> (serializers, "application/x-route-request");
    add_int_serializer<route_reply_t> (serializers, "application/x-route-reply");
    auto builder = test::runtime_failure_builder ();
    auto runtime = detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    unsigned copies = 0, sends = 0, requests = 0;
    runtime.bind_mesh_node_transport (
      "copy-mesh",
      [counter = transport_copy_counter_t (copies),
       &sends] (const zlink::routing_id_t &, message_parts_t) -> task_t<result_t<void>> {
          ++sends;
          co_return result_t<void>::success ();
      },
      [counter = transport_copy_counter_t (copies),
       &requests] (const zlink::routing_id_t &, message_parts_t,
                   std::chrono::milliseconds) -> request_task_t<result_t<message_parts_t>> {
          ++requests;
          co_return result_t<message_parts_t>::failure (framework_error_kind_t::not_found,
                                                        "test target is absent");
      });
    auto client = builder.route_client (serializers);
    copies = 0;
    EXPECT_TRUE (
      client.send_to_node ("copy-mesh", zlink::routing_id_t::from ("target"), route_event_t{7})
        .async ()
        .result ());
    EXPECT_EQ (sends, 1u);
    EXPECT_EQ (copies, 0u) << "Send copied the registered transport callable";
    copies = 0;
    const auto reply =
      client.request_to_node ("copy-mesh", zlink::routing_id_t::from ("target"), route_request_t{7})
        .async<route_reply_t> ()
        .result ();
    EXPECT_EQ (reply.error_kind (), framework_error_kind_t::not_found);
    EXPECT_EQ (requests, 1u);
    EXPECT_EQ (copies, 0u) << "Request copied the registered transport callable";
}

TEST (CppFrameworkRouteClientChannelRouting, PendingNodeRequestRetainsReplacedTransport)
{
    using namespace zlink::framework;
    using runtime::messaging::message_parts_t;
    serializer_registry_t serializers;
    add_int_serializer<route_request_t> (serializers, "application/x-route-request");
    add_int_serializer<route_reply_t> (serializers, "application/x-route-reply");
    auto builder = test::runtime_failure_builder ();
    auto runtime = detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    auto lifetime = std::make_shared<int> ();
    std::weak_ptr<int> retained = lifetime;
    task_completion_source_t<result_t<message_parts_t>> physical_reply;
    runtime.bind_mesh_node_transport (
      "held-mesh", {},
      [lifetime,
       &physical_reply] (const zlink::routing_id_t &, message_parts_t,
                         std::chrono::milliseconds) -> request_task_t<result_t<message_parts_t>> {
          co_return co_await physical_reply.task ();
      });
    lifetime.reset ();
    {
        auto client = builder.route_client (serializers);
        auto pending =
          client
            .request_to_node ("held-mesh", zlink::routing_id_t::from ("target"), route_request_t{7})
            .async<route_reply_t> ();
        runtime.bind_mesh_node_transport ("held-mesh", {}, {});
        EXPECT_FALSE (retained.expired ());
        physical_reply.complete (
          result_t<result_t<message_parts_t>>::success (result_t<message_parts_t>::failure (
            framework_error_kind_t::not_found, "test target is absent")));
        EXPECT_EQ (pending.result ().error_kind (), framework_error_kind_t::not_found);
    }
    // Submit and completion spec §11 permits independent completion before the
    // producer stack returns. Observing the result does not join transport cleanup.
    EXPECT_TRUE (wait_until ([&] { return retained.expired (); }));
}

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
        -> zlink::framework::request_task_t<zlink::framework::result_t<message_parts_t>> {
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
          co_await zlink::framework::detail::request_admitted_t{};
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
              return zlink::framework::request_task_t<zlink::message_t> (
                zlink::framework::result_t<zlink::message_t>::failure (
                  framework_error_kind_t::internal_failure, client_server_request_error));
          }
          ++client_server_request_count;
          const auto serialized =
            serializers.get<route_reply_t> ().serialize_with_content_type (route_reply_t{38});
          return zlink::framework::request_task_t<zlink::message_t> (
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

namespace
{
class admitted_spot_resolver_t final : public zlink::framework::runtime::spot_address_resolver_t
{
  public:
    zlink::framework::task_t<std::optional<zlink::framework::runtime::spot_address_t>>
    resolve_spot_address (std::string, std::string spot_id) override
    {
        using namespace zlink::framework;
        runtime::spot_address_t address;
        address.mesh_name = "admission-mesh";
        address.node_rid = zlink::routing_id_t::from ("target");
        address.spot_id = std::move (spot_id);
        address.spot_generation = 1;
        return task_t<std::optional<runtime::spot_address_t>> (
          result_t<std::optional<runtime::spot_address_t>>::success (std::move (address)));
    }
    void invalidate_spot_address (std::string_view) override {}
    void invalidate_all_routes_after_store_recovery () override {}
};

void verify_mesh_request_admission (bool to_spot)
{
    using namespace zlink::framework;
    using runtime::messaging::envelope_codec_t;
    using runtime::messaging::message_kind_t;
    using runtime::messaging::message_parts_t;
    serializer_registry_t serializers;
    add_int_serializer<route_request_t> (serializers, "application/x-route-request");
    add_int_serializer<route_reply_t> (serializers, "application/x-route-reply");
    auto builder = test::runtime_failure_builder ();
    auto runtime = detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    auto accepted = std::make_shared<task_completion_source_t<void>> ();
    task_completion_source_t<result_t<message_parts_t>> physical_reply;
    std::optional<message_parts_t> response;
    auto submit = [&] (message_parts_t parts) -> request_task_t<result_t<message_parts_t>> {
        envelope_codec_t envelope;
        auto header = envelope.decode_header (parts, false);
        EXPECT_TRUE (header);
        if (header) {
            auto reply_header = header.value ();
            reply_header.kind = message_kind_t::response;
            const auto encoded =
              serializers.get<route_reply_t> ().serialize_with_content_type (route_reply_t{14});
            reply_header.content_type = encoded.content_type;
            response.emplace (envelope.encode_raw_body_parts (
              reply_header, detail::encoded_payload_to_raw (encoded.payload)));
        }
        return request_task_t<result_t<message_parts_t>> (physical_reply.task (), accepted);
    };
    runtime.bind_mesh_node_transport (
      "admission-mesh", {},
      [&] (const zlink::routing_id_t &, message_parts_t parts, std::chrono::milliseconds) {
          return submit (std::move (parts));
      });
    runtime.bind_spot_mesh_transport ("admission-mesh", {},
                                      [&] (const zlink::routing_id_t &, const std::string &spot_id,
                                           std::uint64_t generation, message_parts_t parts,
                                           std::chrono::milliseconds) {
                                          EXPECT_EQ (spot_id, "admission-spot");
                                          EXPECT_EQ (generation, 1);
                                          return submit (std::move (parts));
                                      });
    admitted_spot_resolver_t resolver;
    runtime.bind_spot_address_resolver (resolver);
    auto client = builder.route_client (serializers);
    auto pending = to_spot
                     ? client.request_to_spot (spot_id_t ("admission-spot"), route_request_t{7})
                         .async<route_reply_t> ()
                     : client
                         .request_to_node ("admission-mesh", zlink::routing_id_t::from ("target"),
                                           route_request_t{7})
                         .async<route_reply_t> ();
    auto admitted = pending.admitted ();
    EXPECT_FALSE (admitted.await_ready ());
    EXPECT_FALSE (pending.await_ready ());
    accepted->complete (result_t<void>::success ());
    EXPECT_TRUE (admitted.await_ready ()) << "Mesh request waited for reply to publish admission";
    EXPECT_FALSE (pending.await_ready ());
    ASSERT_TRUE (response.has_value ());
    physical_reply.complete (result_t<result_t<message_parts_t>>::success (
      result_t<message_parts_t>::success (std::move (*response))));
    ASSERT_TRUE (pending.result ());
    EXPECT_EQ (pending.result ().value ().value, 14);
    EXPECT_TRUE (admitted.result ());
}

} // namespace

TEST (CppFrameworkRouteClientChannelRouting, NodeRequestPublishesItsOwnAdmissionBeforeReply)
{
    verify_mesh_request_admission (false);
}

TEST (CppFrameworkRouteClientChannelRouting, SpotRequestPublishesItsOwnAdmissionBeforeReply)
{
    verify_mesh_request_admission (true);
}

namespace
{
struct spot_lookup_copy_observer_t
{
    explicit spot_lookup_copy_observer_t (unsigned &lane_copies) : lane_copies (&lane_copies) {}
    spot_lookup_copy_observer_t (const spot_lookup_copy_observer_t &other) :
        lane_copies (other.lane_copies)
    {
        if (zlink::framework::runtime::state_lane_t::current ())
            ++*lane_copies;
    }
    spot_lookup_copy_observer_t (spot_lookup_copy_observer_t &&) noexcept = default;
    unsigned *lane_copies;
};
} // namespace

TEST (CppFrameworkRouteClientChannelRouting, SpotSenderLookupDoesNotEnterStateLane)
{
    using namespace zlink::framework;
    using runtime::messaging::message_parts_t;
    serializer_registry_t serializers;
    add_int_serializer<route_event_t> (serializers, "application/x-route-event");
    auto builder = test::runtime_failure_builder ();
    auto channel = detail::channel_runtime_t::from (builder.message_bus ());
    channel.bind_serializers (serializers);
    admitted_spot_resolver_t resolver;
    channel.bind_spot_address_resolver (resolver);
    runtime::offload_executor_t target_executor{1, "spot-target-admission"};
    runtime::state_lane_t target_lane{target_executor};
    unsigned lookup_lane_copies = 0, target_admissions = 0;
    channel.bind_spot_mesh_transport (
      "admission-mesh",
      [observer = spot_lookup_copy_observer_t (lookup_lane_copies), &target_lane,
       &target_admissions] (const zlink::routing_id_t &node, const std::string &spot,
                            std::uint64_t generation, message_parts_t) -> task_t<result_t<void>> {
          EXPECT_EQ (node.to_string (), "target");
          EXPECT_EQ (spot, "admission-spot");
          EXPECT_EQ (generation, 1u);
          co_return co_await target_lane.run_task ([&] {
              ++target_admissions;
              return result_t<void>::success ();
          });
      },
      {});
    auto client = builder.route_client (serializers);
    lookup_lane_copies = 0;
    constexpr unsigned submits = 1000;
    for (unsigned i = 0; i < submits; ++i)
        ASSERT_TRUE (client.send_to_spot ("admission-spot", route_event_t{7}).async ().result ());
    EXPECT_EQ (target_admissions, submits);
    EXPECT_EQ (lookup_lane_copies, 0u)
      << "one sender copy inside the channel lane identifies each lookup turn";
    std::cout << "spot submits=" << submits
              << " lookup_lane_copy_observations=" << lookup_lane_copies
              << " target_admission_turns=" << target_admissions << '\n';
}

TEST (CppFrameworkRouteClientChannelRouting, PendingSpotSendRetainsTransportThroughRebindAndClose)
{
    using namespace zlink::framework;
    using runtime::messaging::message_parts_t;
    serializer_registry_t serializers;
    add_int_serializer<route_event_t> (serializers, "application/x-route-event");
    auto builder = test::runtime_failure_builder ();
    auto channel = detail::channel_runtime_t::from (builder.message_bus ());
    channel.bind_serializers (serializers);
    admitted_spot_resolver_t resolver;
    channel.bind_spot_address_resolver (resolver);
    auto lifetime = std::make_shared<int> (7);
    std::weak_ptr<int> retained = lifetime;
    task_completion_source_t<result_t<void>> target_admission;
    bool entered = false;
    channel.bind_spot_mesh_transport ("admission-mesh",
                                      [lifetime, &target_admission, &entered] (
                                        const zlink::routing_id_t &, const std::string &,
                                        std::uint64_t, message_parts_t) -> task_t<result_t<void>> {
                                          entered = true;
                                          auto result = co_await target_admission.task ();
                                          EXPECT_EQ (*lifetime, 7);
                                          co_return result;
                                      },
                                      {});
    lifetime.reset ();
    auto client = builder.route_client (serializers);
    {
        auto pending = client.send_to_spot ("admission-spot", route_event_t{7}).async ();
        EXPECT_TRUE (entered);
        unsigned replacement_submits = 0;
        channel.bind_spot_mesh_transport (
          "admission-mesh",
          [&replacement_submits] (const zlink::routing_id_t &, const std::string &, std::uint64_t,
                                  message_parts_t) -> task_t<result_t<void>> {
              ++replacement_submits;
              co_return result_t<void>::success ();
          },
          {});
        EXPECT_FALSE (retained.expired ());
        EXPECT_TRUE (client.send_to_spot ("admission-spot", route_event_t{8}).async ().result ());
        EXPECT_EQ (replacement_submits, 1u);
        channel.close ();
        EXPECT_FALSE (retained.expired ());
        target_admission.complete (result_t<result_t<void>>::success (result_t<void>::failure (
          framework_error_kind_t::shutting_down, "target admission closed")));
        EXPECT_EQ (pending.result ().error_kind (), framework_error_kind_t::shutting_down);
    }
    // Same Submit and completion §11 rule as PendingNodeRequestRetainsReplacedTransport.
    EXPECT_TRUE (wait_until ([&] { return retained.expired (); }));
}

TEST (CppFrameworkRouteClientChannelRouting, MissingSpotTransportRemainsUnavailable)
{
    using namespace zlink::framework;
    serializer_registry_t serializers;
    add_int_serializer<route_event_t> (serializers, "application/x-route-event");
    auto builder = test::runtime_failure_builder ();
    auto channel = detail::channel_runtime_t::from (builder.message_bus ());
    channel.bind_serializers (serializers);
    admitted_spot_resolver_t resolver;
    channel.bind_spot_address_resolver (resolver);
    const auto result = builder.route_client (serializers)
                          .send_to_spot ("admission-spot", route_event_t{7})
                          .async ()
                          .result ();
    EXPECT_EQ (result.error_kind (), framework_error_kind_t::unavailable);
}
namespace
{
struct publication_copy_action_t
{
    explicit publication_copy_action_t (std::function<void ()> action) : action (std::move (action))
    {
    }
    publication_copy_action_t (const publication_copy_action_t &other) : action (other.action)
    {
        action ();
    }
    publication_copy_action_t (publication_copy_action_t &&) noexcept = default;
    std::function<void ()> action;
};
} // namespace

TEST (CppFrameworkRouteClientChannelRouting, SpotSenderPublicationRetriesWithoutLosingInputs)
{
    using namespace zlink::framework;
    using runtime::messaging::message_parts_t;
    auto state = std::make_shared<detail::channel_runtime_state_t> ();
    detail::channel_runtime_t channel (state);
    bool compete = false;
    unsigned competing_publications = 0, sender_copies = 0, sends = 0;
    std::promise<void> prepared, release;
    auto prepared_future = prepared.get_future ();
    const auto released = release.get_future ().share ();
    bool signalled = false;
    std::exception_ptr bind_error;
    channel.bind_spot_mesh_transport (
      "existing",
      [action = publication_copy_action_t ([&] {
           if (std::exchange (compete, false)) {
               ++competing_publications;
               state->publish_node_transports (
                 [] (auto &index) { index.route_channels.insert ("concurrent-route"); });
               EXPECT_TRUE (state->spot_mesh_requesters.contains ("retry"));
               signalled = true;
               prepared.set_value ();
               released.wait ();
           }
       })] (const zlink::routing_id_t &, const std::string &, std::uint64_t,
            message_parts_t) -> task_t<result_t<void>> { co_return result_t<void>::success (); },
      {});
    compete = true;
    std::jthread binder ([&] {
        try {
            channel.bind_spot_mesh_transport (
              "retry",
              [action = publication_copy_action_t ([&] {
                   ++sender_copies;
                   if (runtime::state_lane_t::current ())
                       EXPECT_TRUE (state->spot_mesh_requesters.contains ("retry"));
               }),
               &sends] (const zlink::routing_id_t &, const std::string &, std::uint64_t,
                        message_parts_t) -> task_t<result_t<void>> {
                  ++sends;
                  co_return result_t<void>::success ();
              },
              {});
        }
        catch (...) {
            bind_error = std::current_exception ();
            if (!signalled)
                prepared.set_exception (bind_error);
        }
    });
    {
        struct release_on_exit_t
        {
            std::promise<void> &release;
            ~release_on_exit_t () { release.set_value (); }
        } release_on_exit{release};
        prepared_future.get ();
        const auto before =
          std::atomic_load_explicit (&state->node_transports, std::memory_order_acquire);
        EXPECT_TRUE (before->route_channels.contains ("concurrent-route"));
        EXPECT_FALSE (before->spot_mesh_senders.contains ("retry"));
    }
    binder.join ();
    if (bind_error)
        std::rethrow_exception (bind_error);
    const auto index =
      std::atomic_load_explicit (&state->node_transports, std::memory_order_acquire);
    ASSERT_TRUE (index->route_channels.contains ("concurrent-route"));
    ASSERT_TRUE (index->spot_mesh_senders.contains ("existing"));
    ASSERT_TRUE (index->spot_mesh_senders.contains ("retry"));
    EXPECT_EQ (competing_publications, 1u);
    EXPECT_GE (sender_copies, 2u);
    const auto &sender = index->spot_mesh_senders.at ("retry");
    EXPECT_TRUE (static_cast<bool> (sender));
    if (sender)
        EXPECT_TRUE (sender (zlink::routing_id_t::from ("target"), "spot", 1, {}).result ());
    EXPECT_EQ (sends, 1u);
    std::cout << "competing_publications=" << competing_publications
              << " sender_copies=" << sender_copies << " published_sender_submits=" << sends
              << '\n';
}

TEST (CppFrameworkRouteClientChannelRouting, BothRouteInitializersPublishAfterPreparation)
{
    using namespace zlink::framework;
    for (const bool use_registration : {false, true}) {
        auto state = std::make_shared<detail::channel_runtime_state_t> ();
        detail::channel_runtime_t channel (state);
        detail::channel_runtime_manager_t manager (state);
        std::promise<void> prepared, release;
        auto prepared_future = prepared.get_future ();
        const auto released = release.get_future ().share ();
        bool pause = false;
        channel.bind_spot_mesh_transport (
          "existing",
          [action = publication_copy_action_t ([&] {
               if (std::exchange (pause, false)) {
                   prepared.set_value ();
                   released.wait ();
               }
           })] (const zlink::routing_id_t &, const std::string &, std::uint64_t,
                runtime::messaging::message_parts_t) -> task_t<result_t<void>> {
              co_return result_t<void>::success ();
          },
          {});
        const auto before = std::atomic_load (&state->node_transports);
        pause = true;
        std::exception_ptr initialization_error;
        std::jthread initializer ([&] {
            try {
                if (use_registration) {
                    std::map<std::string, std::shared_ptr<detail::route_channel_builder_state_t>>
                      registrations;
                    registrations.emplace (
                      "published",
                      std::make_shared<detail::route_channel_builder_state_t> ("published"));
                    manager.initialize_route_channels (registrations);
                } else {
                    manager.initialize_route_channels (std::vector<std::string>{"published"});
                }
            }
            catch (...) {
                initialization_error = std::current_exception ();
                prepared.set_exception (initialization_error);
            }
        });
        {
            struct release_on_exit_t
            {
                std::promise<void> &release;
                ~release_on_exit_t () { release.set_value (); }
            } release_on_exit{release};
            prepared_future.get ();
            EXPECT_TRUE (state->route_channels.contains ("published"));
            EXPECT_TRUE (state->route_channels.at ("published")->running ());
            EXPECT_EQ (std::atomic_load (&state->node_transports), before);
            EXPECT_FALSE (before->route_channels.contains ("published"));
        }
        initializer.join ();
        if (initialization_error)
            std::rethrow_exception (initialization_error);
        const auto after = std::atomic_load (&state->node_transports);
        EXPECT_TRUE (after->route_channels.contains ("published"));
        EXPECT_TRUE (after->spot_mesh_senders.contains ("existing"));
        EXPECT_FALSE (before->route_channels.contains ("published"));
        manager.initialize_route_channels (std::vector<std::string>{"published"});
        EXPECT_EQ (std::atomic_load (&state->node_transports), after);
        channel.close ();
        EXPECT_TRUE (
          std::atomic_load (&state->node_transports)->route_channels.contains ("published"));
    }
}

TEST (CppFrameworkRouteClientChannelRouting, RoutePresenceKeepsPriorityOverReboundSpotMesh)
{
    using namespace zlink::framework;
    using runtime::messaging::message_parts_t;
    serializer_registry_t serializers;
    add_int_serializer<route_event_t> (serializers, "application/x-route-event");
    add_int_serializer<route_request_t> (serializers, "application/x-route-request");
    add_int_serializer<route_reply_t> (serializers, "application/x-route-reply");
    auto builder = test::runtime_failure_builder ();
    auto channel = detail::channel_runtime_t::from (builder.message_bus ());
    channel.bind_serializers (serializers);
    admitted_spot_resolver_t resolver;
    channel.bind_spot_address_resolver (resolver);
    auto manager = detail::channel_runtime_manager_t::from (builder);
    manager.initialize_route_channels (std::vector<std::string>{"admission-mesh"});
    // A stopped route gives an immediate, identifiable rejection without waiting
    // for a peer. Its presence must still take precedence over the mesh transport.
    manager.get_route_channel ("admission-mesh").stop ();
    unsigned mesh_calls = 0;
    auto client = builder.route_client (serializers);
    for (unsigned binding = 0; binding != 2; ++binding) {
        channel.bind_spot_mesh_transport (
          "admission-mesh",
          [&] (const zlink::routing_id_t &, const std::string &, std::uint64_t,
               message_parts_t) -> task_t<result_t<void>> {
              ++mesh_calls;
              co_return result_t<void>::success ();
          },
          [&] (const zlink::routing_id_t &, const std::string &, std::uint64_t, message_parts_t,
               std::chrono::milliseconds) -> request_task_t<result_t<message_parts_t>> {
              ++mesh_calls;
              co_return result_t<message_parts_t>::failure (framework_error_kind_t::not_found,
                                                            "mesh selected");
          });
        const auto sent =
          client.send_to_spot ("admission-spot", route_event_t{7}).async ().result ();
        EXPECT_EQ (sent.error_kind (), framework_error_kind_t::unavailable);
        EXPECT_EQ (error_message (sent), "route channel runtime is not running");
        const auto reply = client.request_to_spot ("admission-spot", route_request_t{7})
                             .async<route_reply_t> ()
                             .result ();
        EXPECT_EQ (reply.error_kind (), framework_error_kind_t::unavailable);
        EXPECT_EQ (error_message (reply), "route channel runtime is not running");
        EXPECT_EQ (mesh_calls, 0u);
    }
    channel.close ();
    EXPECT_EQ (
      client.send_to_spot ("admission-spot", route_event_t{7}).async ().result ().error_kind (),
      framework_error_kind_t::unavailable);
    EXPECT_EQ (mesh_calls, 0u);
}
