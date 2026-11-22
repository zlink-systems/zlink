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
#include <google/protobuf/wrappers.pb.h>
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
struct outer_route_request_t
{
    static constexpr const char *packet_name = "outer";
    int value{};
};

struct inner_route_request_t
{
    static constexpr const char *packet_name = "inner";
    int value{};
};
} // namespace

template <> struct zlink::json::dto_traits<inner_route_request_t>
{
    static constexpr auto fields =
      std::tuple{zlink::json::field ("value", &inner_route_request_t::value)};
};

namespace
{
struct api_hop_request_t
{
    static constexpr const char *packet_name = "api-hop";
    int value{};
};
} // namespace

template <> struct zlink::json::dto_traits<api_hop_request_t>
{
    static constexpr auto fields =
      std::tuple{zlink::json::field ("value", &api_hop_request_t::value)};
};

namespace
{
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
} // namespace

namespace framework_channel_messaging_tests
{
int verify_outbound_submission (
  zlink::framework::zlink_builder_t &zlink,
  zlink::framework::zlink_builder_t &outbound_only,
  zlink::framework::detail::channel_runtime_t &outbound_runtime,
  zlink::framework::zlink_builder_t &local_server,
  std::vector<zlink::framework::message_dispatch_error_event_t> &dispatch_errors,
  std::mutex &dispatch_errors_mutex,
  const std::filesystem::path &dispatch_log_path,
  zlink::framework::dispatch_options_t &local_dispatch)
{
    zlink::framework::service_collection_t services;
    services.add_singleton<local_handler_state_t> ();
    services.add_singleton<route_filter_state_t> ();
    auto provider = services.build_provider ();

    zlink::framework::serializer_registry_t serializers;
    std::atomic_int request_deserialize_count{0};
    serializers.add<request_t> (
      [] (const request_t &value) {
          return zlink::framework::encoded_payload_t::from_string (std::to_string (value.value));
      },
      [&request_deserialize_count] (const zlink::framework::encoded_payload_t &payload) {
          request_deserialize_count.fetch_add (1, std::memory_order_relaxed);
          return request_t{std::stoi (payload.to_string ())};
      },
      "application/json");
    add_int_serializer<outer_route_request_t> (serializers,
                                               "application/x-zlink-test-outer-request");
    add_int_serializer<inner_route_request_t> (serializers,
                                               "application/x-zlink-test-inner-request");
    add_int_serializer<api_hop_request_t> (serializers, "application/x-zlink-test-api-hop-request");
    add_int_serializer<missing_probe_request_t> (serializers,
                                                 "application/x-zlink-test-missing-probe-request");
    add_int_serializer<reply_t> (serializers, "application/x-zlink-test-reply");
    add_int_serializer<event_t> (serializers, "application/x-zlink-test-event");

    // A generated protobuf type is selected through the extension marker, not
    // a per-message registry entry. The ClientServer wire metadata must come
    // from the same typed serializer that produced the bytes.
    zlink::framework::serializer_registry_t protobuf_serializers;
    zlink::framework::codec_registration_context_t protobuf_registration (protobuf_serializers);
    zlink::framework_codecs::protobuf ().register_framework_codecs (protobuf_registration);
    for (const auto size : {0u, 1u, 1024u, 4096u}) {
        google::protobuf::StringValue value;
        value.set_value (std::string (size, 'x'));
        const auto serializer = protobuf_serializers.get<google::protobuf::StringValue> ();
        zlink::message_t retained;
        {
            const auto encoded = serializer.serialize (value);
            assert (encoded.to_string () == value.SerializeAsString ());
            retained = zlink::framework::detail::encoded_payload_to_raw (encoded);
            if (size >= 1024)
                assert (retained.data () == encoded.bytes ().data ());
            assert (serializer.deserialize (encoded).value () == value.value ());
        }
        assert (retained.to_string () == value.SerializeAsString ());
    }
    zlink::framework::zlink_builder_t protobuf_client_builder =
      zlink::framework::test::runtime_failure_builder ();
    protobuf_client_builder.channel ("protobuf-client")
      .enable_client ()
      .connect ("tcp://127.0.0.1:1");
    auto protobuf_client_runtime =
      zlink::framework::detail::channel_runtime_t::from (protobuf_client_builder.message_bus ());
    protobuf_client_runtime.bind_serializers (protobuf_serializers);
    std::string observed_protobuf_packet_name;
    std::string observed_protobuf_content_type;
    std::string observed_protobuf_payload;
    bool protobuf_strict_decode_succeeded = false;
    protobuf_client_runtime.bind_client_server_transport (
      "protobuf-client",
      [] (std::string, std::string, zlink::message_t, std::map<std::string, std::string>) {
          return zlink::framework::task_t<void> (zlink::framework::result_t<void>::success ());
      },
      [&] (std::string packet_name, std::string content_type, zlink::message_t payload,
           std::chrono::milliseconds, std::map<std::string, std::string>) {
          observed_protobuf_packet_name = std::move (packet_name);
          observed_protobuf_content_type = content_type;
          observed_protobuf_payload = payload.to_string ();
          const auto decoded =
            zlink::framework::detail::deserialize_typed_payload<google::protobuf::StringValue> (
              protobuf_serializers, payload, content_type);
          protobuf_strict_decode_succeeded = decoded.value () == "direct-protobuf";
          const auto reply =
            protobuf_serializers.get<google::protobuf::StringValue> ().serialize (decoded);
          return zlink::framework::request_task_t<zlink::message_t> (
            zlink::framework::result_t<zlink::message_t>::success (
              zlink::framework::detail::encoded_payload_to_raw (reply)));
      });
    google::protobuf::StringValue protobuf_request;
    protobuf_request.set_value ("direct-protobuf");
    const auto expected_protobuf_payload =
      protobuf_serializers.get<google::protobuf::StringValue> ()
        .serialize (protobuf_request)
        .to_string ();
    const auto protobuf_reply = protobuf_client_builder.message_bus ()
                                  .request ("protobuf-client", protobuf_request)
                                  .async<google::protobuf::StringValue> ()
                                  .result ();
    if (!protobuf_reply || protobuf_reply.value ().value () != "direct-protobuf"
        || observed_protobuf_packet_name != "StringValue"
        || observed_protobuf_content_type != "application/x-protobuf"
        || observed_protobuf_payload != expected_protobuf_payload
        || !protobuf_strict_decode_succeeded) {
        return 413;
    }

    const auto cancellation =
      zlink::framework::detail::make_cancellation_exception ("channel transport cancelled");
    constexpr auto cancellation_channel = "cancelled-client";
    zlink::framework::zlink_builder_t cancellation_client_builder =
      zlink::framework::test::runtime_failure_builder ();
    cancellation_client_builder.channel (cancellation_channel)
      .enable_client ()
      .connect ("tcp://127.0.0.1:1");
    auto cancellation_client_runtime = zlink::framework::detail::channel_runtime_t::from (
      cancellation_client_builder.message_bus ());
    cancellation_client_runtime.bind_serializers (protobuf_serializers);
    cancellation_client_runtime.bind_client_server_transport (
      cancellation_channel,
      [cancellation] (std::string, std::string, zlink::message_t,
                      std::map<std::string, std::string>) {
          return zlink::framework::task_t<void> (
            zlink::framework::detail::result_access_t::failure<void> (cancellation));
      },
      [cancellation] (std::string, std::string, zlink::message_t, std::chrono::milliseconds,
                      std::map<std::string, std::string>) {
          return zlink::framework::request_task_t<zlink::message_t> (
            zlink::framework::detail::result_access_t::failure<zlink::message_t> (cancellation));
      });
    const auto cancelled_request = cancellation_client_builder.message_bus ()
                                     .request (cancellation_channel, protobuf_request)
                                     .async<google::protobuf::StringValue> ()
                                     .result ();
    const auto cancelled_send = cancellation_client_builder.message_bus ()
                                  .send (cancellation_channel, protobuf_request)
                                  .async ()
                                  .result ();
    assert (!cancelled_request && !cancelled_send);
    assert (cancelled_request.error () == nullptr && cancelled_send.error () == nullptr);
    // C++ common runtime §7.4 preserves the exception; MSVC may copy it on rethrow.
    try {
        std::rethrow_exception (cancellation);
    }
    catch (const std::system_error &original) {
        assert (original.code () == std::errc::operation_canceled);
        for (const auto &exception :
             {cancelled_request.exception (), cancelled_send.exception ()}) {
            assert (exception);
            try {
                std::rethrow_exception (exception);
            }
            catch (const std::system_error &error) {
                assert (typeid (error) == typeid (original));
                assert (error.code () == original.code ());
                assert (std::string (error.what ()) == original.what ());
            }
        }
    }

    zlink::framework::detail::channel_runtime_t::from (outbound_only.message_bus ())
      .bind_serializers (serializers);
    zlink::framework::zlink_builder_t shutdown_outbound =
      zlink::framework::test::runtime_failure_builder ();
    auto shutdown_channel = shutdown_outbound.channel ("shutdown-client");
    shutdown_channel.enable_client ().connect ("tcp://127.0.0.1:1");
    shutdown_channel.enable_publisher ().connect ("tcp://127.0.0.1:2");
    auto shutdown_runtime =
      zlink::framework::detail::channel_runtime_t::from (shutdown_outbound.message_bus ());
    shutdown_runtime.bind_serializers (serializers);
    shutdown_runtime.shutdown ();
    /* One-way send and publish report shutdown on the returned terminal instead of
     * throwing out of submit(): common spec 04-async-execution-policy.ko.md §1.3 says the
     * one-way terminator "실패하면 예외로 완료한다", and the failure table in the same
     * section maps "runtime이 새 admission을 받지 않음" to the public kind
     * `RuntimeShutdown`(`36`). The internal boundary facet is no longer the observable
     * surface, so the terminal is asserted on the public kind. */
    const auto completes_shutdown = [] (auto &&submit_fn) {
        const auto submitted = submit_fn ().result ();
        return !submitted
               && submitted.error_kind ()
                    == zlink::framework::framework_error_kind_t::shutting_down;
    };
    if (!completes_shutdown ([&] {
            return shutdown_outbound.message_bus ().send ("shutdown-client", event_t{1}).async ();
        })
        || !completes_shutdown ([&] {
               return shutdown_outbound.message_bus ()
                 .publish ("shutdown-client", "events", event_t{2})
                 .async ();
           })) {
        return 407;
    }
    auto outbound_only_request = outbound_only.message_bus ()
                                   .request ("client-only", request_t{6})
                                   .async<reply_t> ()
                                   .result ();
    if (outbound_only_request
        || (outbound_only_request.error () != nullptr
            && zlink::framework::detail::boundary_state (*outbound_only_request.error ())
                 != zlink::framework::detail::boundary_error_t::disconnected)) {
        return 404;
    }

    zlink::framework::channel_request_call_t unbound_route_request ("request", nullptr, {});
    const auto unbound_route_request_result = unbound_route_request.async<reply_t> ().result ();
    if (unbound_route_request_result
        || unbound_route_request_result.error_kind ()
             != zlink::framework::framework_error_kind_t::protocol_error) {
        return 406;
    }
    zlink::framework::route_client_t unconfigured_route_client;
    const auto unconfigured_target = zlink::routing_id_t::from (std::string ("unconfigured-node"));
    const zlink::framework::spot_id_t unconfigured_spot = "unconfigured-spot";
    /* Same one-way terminal rule as the default-constructed client above: common spec
     * 04-async-execution-policy.ko.md §1.3 completes the failure on the returned task. */
    const auto route_send_rejected = [] (auto &&send_fn) {
        const auto submitted = send_fn ().result ();
        return !submitted
               && submitted.error_kind ()
                    == zlink::framework::framework_error_kind_t::protocol_error;
    };
    if (!route_send_rejected ([&] {
            return unconfigured_route_client
              .send_to_node ("game.route", unconfigured_target, event_t{7})
              .async ();
        })) {
        return 409;
    }
    const auto unconfigured_route_request =
      unconfigured_route_client.request_to_node ("game.route", unconfigured_target, request_t{8})
        .async<reply_t> ()
        .result ();
    if (unconfigured_route_request
        || unconfigured_route_request.error_kind ()
             != zlink::framework::framework_error_kind_t::protocol_error) {
        return 408;
    }
    const auto unconfigured_spot_send =
      unconfigured_route_client.send_to_spot (unconfigured_spot, event_t{9}).async ().result ();
    if (unconfigured_spot_send
        || unconfigured_spot_send.error_kind ()
             != zlink::framework::framework_error_kind_t::not_configured) {
        return 410;
    }
    const auto unconfigured_spot_request =
      unconfigured_route_client.request_to_spot (unconfigured_spot, request_t{10})
        .async<reply_t> ()
        .result ();
    if (unconfigured_spot_request
        || unconfigured_spot_request.error_kind ()
             != zlink::framework::framework_error_kind_t::not_configured) {
        return 410;
    }

    return verify_channel_dispatch (zlink, outbound_runtime, local_server, dispatch_errors,
                                    dispatch_errors_mutex, dispatch_log_path, local_dispatch,
                                    provider, serializers, request_deserialize_count);
}
} // namespace framework_channel_messaging_tests
