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
int verify_actor_dispatch (zlink::framework::service_provider_t &provider,
                           zlink::framework::serializer_registry_t &serializers,
                           zlink::framework::runtime::messaging::envelope_codec_t &envelope_codec)
{
    zlink::framework::zlink_builder_t stream_builder =
      zlink::framework::test::runtime_failure_builder ();
    stream_builder.stream ("routed-bound-session").bind ("tcp://0.0.0.0:9300");
    auto stream_runtime = zlink::framework::detail::stream_runtime_t::from (stream_builder);
    auto stream = stream_runtime.open_session ("routed-bound-session");
    zlink::framework::detail::actor_gateway_runtime_t actor_gateway;
    auto actor_ref = zlink::framework::detail::actor_ref_access_t::make (
      zlink::framework::node_rid_t::from_string ("play-node"), "PlayerActor", "observer", 3);
    auto bound_actor = actor_gateway.manager ().bind (actor_ref).async ().result ();
    if (!bound_actor) {
        return 66;
    }
    actor_gateway.bind_session_stream ("observer", stream, zlink::framework::stream_codec_t::json);
    zlink::framework::spot_node_builder_t actor_route_spot_builder;
    auto actor_route_spot_runtime =
      zlink::framework::detail::spot_node_runtime_t::from (actor_route_spot_builder);
    zlink::framework::detail::spot_route_internal_dispatcher_t actor_dispatcher (
      actor_route_spot_runtime, actor_gateway, zlink::framework::route_client_t{}, serializers);
    if (!actor_dispatcher.can_handle_request (
          zlink::framework::detail::actor_bound_session_route_request_t::packet_name)
        || !actor_dispatcher.can_handle_send (
          zlink::framework::detail::actor_bound_session_route_request_t::packet_name)
        || actor_dispatcher.can_handle_request ("not.actor.route")) {
        return 74;
    }
    zlink::framework::runtime::messaging::envelope_header_t bound_header;
    bound_header.kind = zlink::framework::runtime::messaging::message_kind_t::request;
    bound_header.channel_name = "actor.route";
    bound_header.message_name =
      zlink::framework::detail::actor_bound_session_route_request_t::packet_name;
    auto bound_request = zlink::framework::detail::make_actor_bound_session_route_request (
      actor_ref, "BingoRewardAnnouncedNotify", zlink::framework::stream_codec_t::json,
      zlink::message_t::from ("reward"));
    auto bound_parts = envelope_codec.encode_parts (bound_header, bound_request, serializers);
    auto bound_reply = actor_dispatcher.dispatch_request (
      zlink::framework::detail::route_received_packet_t{
        zlink::routing_id_t::from (std::string ("play-node")), 99, std::move (bound_parts)},
      bound_header, provider);
    if (!bound_reply) {
        return 67;
    }
    const auto bound_reply_value =
      serializers.get<zlink::framework::detail::actor_bound_session_route_reply_t> ().deserialize (
        zlink::framework::detail::encoded_payload_from_raw (bound_reply.value ()));
    const auto routed_headers = stream_runtime.written_headers (stream);
    if (!bound_reply_value.accepted || routed_headers.size () != 1) {
        return 69;
    }
    const auto &routed_header = routed_headers[0];
    if (routed_header.kind () != zlink::framework::detail::stream_message_kind_t::send) {
        return 70;
    }
    if (routed_header.codec () != zlink::framework::stream_codec_t::json) {
        return 71;
    }
    if (routed_header.packet_name () != "BingoRewardAnnouncedNotify") {
        return 72;
    }
    bound_header.kind = zlink::framework::runtime::messaging::message_kind_t::command;
    auto bound_send_parts = envelope_codec.encode_parts (bound_header, bound_request, serializers);
    auto bound_send = actor_dispatcher
                        .dispatch_send (
                          zlink::framework::detail::route_received_packet_t{
                            zlink::routing_id_t::from (std::string ("play-node")), 100,
                            std::move (bound_send_parts)},
                          bound_header, provider)
                        .result ()
                        .value ();
    const auto routed_send_headers = stream_runtime.written_headers (stream);
    if (!bound_send || routed_send_headers.size () != 2
        || routed_send_headers[1].codec () != zlink::framework::stream_codec_t::json
        || routed_send_headers[1].packet_name () != "BingoRewardAnnouncedNotify") {
        return 73;
    }
    std::atomic_int route_submit_attempts{0};
    auto one_shot_builder = zlink::framework::test::runtime_failure_builder ();
    auto one_shot_runtime =
      zlink::framework::detail::channel_runtime_t::from (one_shot_builder.message_bus ());
    one_shot_runtime.bind_serializers (serializers);
    one_shot_runtime.bind_mesh_node_transport (
      "one-shot",
      [&] (const zlink::routing_id_t &, zlink::framework::runtime::messaging::message_parts_t) {
          ++route_submit_attempts;
          return zlink::framework::task_t<zlink::framework::result_t<void>> (
            zlink::framework::result_t<zlink::framework::result_t<void>>::success (
              zlink::framework::result_t<void>::success ()));
      },
      {});
    auto one_shot_route =
      one_shot_builder.route_client (serializers)
        .send_to_node ("one-shot", zlink::routing_id_t::from ("one-shot-node"), event_t{1});
    auto copied_route = one_shot_route;
    one_shot_route.async ().result ().value ();
    bool copied_route_rejected = false;
    try {
        (void) copied_route.async ().result ().value ();
    }
    catch (const zlink::framework::framework_exception_t &error) {
        copied_route_rejected =
          error.kind () == zlink::framework::framework_error_kind_t::protocol_error;
    }
    if (!copied_route_rejected || route_submit_attempts.load () != 1) {
        return 149;
    }

    return verify_released_route_client (serializers, envelope_codec);
}
} // namespace framework_channel_messaging_tests
