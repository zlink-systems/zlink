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

#include "../support/channel_messaging_route_fixture.hpp"
#include "../support/channel_messaging_messages.hpp"
#include "../support/channel_messaging_handler_fixture.hpp"

using namespace framework_channel_messaging_fixture;

namespace framework_channel_messaging_tests
{
int verify_route_submission (
  zlink::framework::zlink_builder_t &zlink,
  std::vector<zlink::framework::message_dispatch_error_event_t> &dispatch_errors,
  std::mutex &dispatch_errors_mutex,
  zlink::framework::dispatch_options_t &local_dispatch,
  zlink::framework::service_provider_t &provider,
  zlink::framework::serializer_registry_t &serializers,
  zlink::framework::runtime::messaging::envelope_codec_t &envelope_codec,
  zlink::framework::runtime::messaging::envelope_header_t &request_header,
  zlink::framework::runtime::messaging::message_parts_t &request_parts,
  zlink::framework::runtime::messaging::message_parts_t &route_request_parts,
  std::vector<zlink::framework::message_dispatch_error_event_t> &observed_dispatch_errors)
{
    zlink::framework::detail::route_connection_set_t route_connections;
    if (!route_connections.connect ("tcp://route-b:7500")
        || !route_connections.connect ("tcp://route-a:7500")
        || route_connections.connect ("tcp://route-a:7500")) {
        return 30;
    }
    const auto route_connection_list = route_connections.list ();
    if (route_connection_list.size () != 2 || route_connection_list[0] != "tcp://route-a:7500"
        || route_connection_list[1] != "tcp://route-b:7500") {
        return 31;
    }
    const auto route_peer = zlink::routing_id_t::from (std::string ("route-a"));
    if (!route_connections.connect (route_peer, "tcp://route-a:7500")) {
        return 311;
    }
    const auto route_connection_targets = route_connections.targets ();
    auto upgraded_route_a =
      std::find_if (route_connection_targets.begin (), route_connection_targets.end (),
                    [] (const auto &target) { return target.endpoint == "tcp://route-a:7500"; });
    if (upgraded_route_a == route_connection_targets.end () || !upgraded_route_a->peer_rid
        || *upgraded_route_a->peer_rid != route_peer) {
        return 312;
    }
    if (route_connections.connect (route_peer, "tcp://route-a:7500")) {
        return 313;
    }
    const auto replacement_route_peer =
      zlink::routing_id_t::from (std::string ("route-a-replacement"));
    if (!route_connections.connect (replacement_route_peer, "tcp://route-a:7500")
        || route_connections.disconnect (route_peer, "tcp://route-a:7500")
        || !route_connections.contains ("tcp://route-a:7500")
        || !route_connections.disconnect (replacement_route_peer, "tcp://route-a:7500")) {
        return 314;
    }
    if (!route_connections.disconnect ("tcp://route-b:7500")
        || route_connections.contains ("tcp://route-b:7500")) {
        return 32;
    }

    zlink::framework::detail::route_channel_runtime_t route_runtime ("game.route");
    const auto target_node = zlink::routing_id_t::from (std::string ("remote-node"));
    // Submit §3: this registered runtime has not started accepting admission.
    auto disconnected_send =
      route_runtime.submit_send (target_node, "event", event_t{77}, serializers);
    if (disconnected_send
        || disconnected_send.error_kind ()
             != zlink::framework::framework_error_kind_t::shutting_down) {
        return 33;
    }
    route_runtime.start ();
    route_runtime.connect ("tcp://route-peer:7500");
    auto route_send = route_runtime.submit_send (target_node, "event", event_t{78}, serializers);
    if (!route_send || route_runtime.outbound_packets ().size () != 1
        || route_runtime.outbound_packets ()[0].request_seq.has_value ()) {
        return 34;
    }
    const auto route_send_header =
      envelope_codec.decode_header (route_runtime.outbound_packets ()[0].parts);
    if (!route_send_header
        || route_send_header.value ().kind
             != zlink::framework::runtime::messaging::message_kind_t::command
        || route_send_header.value ().channel_name != "game.route"
        || route_send_header.value ().message_name != "event") {
        return 35;
    }
    auto route_request = route_runtime.submit_request (target_node, "request", request_t{79},
                                                       serializers, std::chrono::milliseconds (25));
    if (!route_request || route_runtime.pending_request_count () != 1
        || route_runtime.outbound_packets ().size () != 2
        || !route_runtime.outbound_packets ()[1].request_seq.has_value ()) {
        return 36;
    }
    const auto route_request_header =
      envelope_codec.decode_header (route_runtime.outbound_packets ()[1].parts);
    if (!route_request_header
        || route_request_header.value ().kind
             != zlink::framework::runtime::messaging::message_kind_t::request
        || route_request_header.value ().channel_name != "game.route"
        || route_request_header.value ().message_name != "request"
        || !route_request_header.value ().deadline.has_value ()) {
        return 37;
    }
    const std::string target_spot = "remote-spot";
    auto spot_request_builder = zlink::framework::test::runtime_failure_builder ();
    spot_request_builder.route_channel ("spot.route").connect ("tcp://spot-route-peer:7500");
    auto spot_request_manager =
      zlink::framework::detail::channel_runtime_manager_t::from (spot_request_builder);
    spot_request_manager.initialize_route_channels (spot_request_builder);
    auto &spot_backend_runtime = spot_request_manager.get_route_channel ("spot.route");
    auto spot_request_resolver = std::make_shared<test_spot_address_resolver_t> ();
    spot_request_resolver->set (target_spot, zlink::framework::runtime::spot_address_t{
                                               "spot.route", target_node, target_spot});
    zlink::framework::detail::channel_runtime_t::from (spot_request_builder.message_bus ())
      .bind_spot_address_resolver (spot_request_resolver);
    auto spot_request_client = spot_request_builder.route_client (serializers);
    zlink::framework::task_completion_source_t<
      zlink::framework::runtime::messaging::message_parts_t>
      spot_reply_source;
    auto spot_admitted = std::make_shared<zlink::framework::task_completion_source_t<void>> ();
    spot_admitted->complete (zlink::framework::result_t<void>::success ());
    bool spot_request_input_matches = false;
    spot_backend_runtime.set_request_backend (
      [&] (const auto &target, const auto &spot, const auto &parts, auto) {
          const auto body = envelope_codec.decode_body (parts);
          spot_request_input_matches =
            target == target_node && spot == target_spot && body
            && serializers.get<request_t> ()
                   .deserialize (zlink::framework::detail::encoded_payload_from_raw (body.value ()))
                   .value
                 == 79;
          return zlink::framework::request_task_t<
            zlink::framework::runtime::messaging::message_parts_t> (spot_reply_source.task (),
                                                                    spot_admitted);
      });
    auto spot_request =
      spot_request_client.request_to_spot (target_spot, request_t{79}).async<reply_t> ();
    if (!spot_request_input_matches || spot_request.await_ready ()
        || route_runtime.pending_request_count () != 1
        || spot_backend_runtime.pending_request_count () != 0
        || spot_backend_runtime.outbound_packets ().back ().target_spot_id != target_spot) {
        return 38;
    }
    zlink::framework::runtime::messaging::envelope_header_t spot_reply_header;
    spot_reply_header.kind = zlink::framework::runtime::messaging::message_kind_t::response;
    spot_reply_header.channel_name = "spot.route";
    spot_reply_header.message_name = request_t::packet_name;
    const reply_t spot_reply_value{79};
    spot_reply_source.complete (
      zlink::framework::result_t<zlink::framework::runtime::messaging::message_parts_t>::success (
        envelope_codec.encode_parts (spot_reply_header, std::type_index (typeid (reply_t)),
                                     &spot_reply_value, serializers)));
    const auto spot_request_result = spot_request.result ();
    if (!spot_request_result || spot_request_result.value ().value != 79
        || spot_backend_runtime.pending_request_count () != 0) {
        return 384;
    }
    int spot_backend_sends = 0;
    int spot_backend_requests = 0;
    spot_backend_runtime.set_send_backend (
      [&] (const zlink::routing_id_t &target, const std::optional<std::string> &spot,
           const zlink::framework::runtime::messaging::message_parts_t &parts) {
          if (target != target_node || spot != target_spot || parts.size () == 0) {
              return zlink::framework::result_t<void>::failure (
                zlink::framework::framework_error_kind_t::internal_failure,
                "unexpected spot send backend input");
          }
          ++spot_backend_sends;
          return zlink::framework::result_t<void>::success ();
      });
    auto spot_request_budget =
      std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::seconds (30));
    auto spot_request_started = std::chrono::steady_clock::now ();
    spot_backend_runtime.set_request_backend (
      [&] (const zlink::routing_id_t &target, const std::optional<std::string> &spot,
           const zlink::framework::runtime::messaging::message_parts_t &parts,
           std::chrono::milliseconds timeout) {
          const auto minimum_remaining = std::chrono::duration_cast<std::chrono::milliseconds> (
            spot_request_started + spot_request_budget - std::chrono::steady_clock::now ());
          if (target != target_node || spot != target_spot || parts.size () == 0
              || timeout <= std::chrono::milliseconds::zero () || timeout > spot_request_budget
              || timeout < minimum_remaining) {
              return zlink::framework::
                result_t<zlink::framework::runtime::messaging::message_parts_t>::failure (
                  zlink::framework::framework_error_kind_t::internal_failure,
                  "unexpected spot request backend input");
          }
          ++spot_backend_requests;
          auto header = envelope_codec.decode_header (parts).value ();
          header.kind = zlink::framework::runtime::messaging::message_kind_t::response;
          auto body = envelope_codec.decode_body (parts).value ();
          return zlink::framework::result_t<zlink::framework::runtime::messaging::message_parts_t>::
            success (envelope_codec.encode_raw_body_parts (std::move (header), std::move (body)));
      });
    if (!spot_backend_runtime.submit_spot_send_parts (target_node, target_spot, request_parts)
           .result ()
        || spot_backend_sends != 1) {
        return 380;
    }
    spot_request_started = std::chrono::steady_clock::now ();
    const auto spot_backend_public_reply =
      spot_request_client.request_to_spot (target_spot, request_t{79})
        .async<request_t> ()
        .result ();
    if (!spot_backend_public_reply || spot_backend_public_reply.value ().value != 79
        || spot_backend_requests != 1 || spot_backend_runtime.pending_request_count () != 0) {
        return 381;
    }
    spot_request_budget = std::chrono::milliseconds (25);
    spot_request_started = std::chrono::steady_clock::now ();
    auto spot_backend_reply = spot_backend_runtime
                                .request_reply_spot_parts (target_node, target_spot, request_parts,
                                                           std::chrono::steady_clock::now ()
                                                             + std::chrono::milliseconds (25))
                                .result ();
    if (!spot_backend_reply || spot_backend_reply.value ().size () != request_parts.size ()
        || spot_backend_requests != 2 || spot_backend_runtime.pending_request_count () != 0) {
        return 382;
    }
    zlink::framework::detail::route_channel_runtime_t auto_backend_runtime ("auto.route");
    auto_backend_runtime.start ();
    int auto_backend_requests = 0;
    auto_backend_runtime.set_request_backend (
      [&] (const zlink::routing_id_t &target, const std::optional<std::string> &spot,
           const zlink::framework::runtime::messaging::message_parts_t &parts,
           std::chrono::milliseconds timeout) {
          if (target != target_node || spot || parts.size () == 0
              || timeout <= std::chrono::milliseconds::zero ()
              || timeout > std::chrono::milliseconds (25)) {
              return zlink::framework::
                result_t<zlink::framework::runtime::messaging::message_parts_t>::failure (
                  zlink::framework::framework_error_kind_t::internal_failure,
                  "unexpected auto route backend input");
          }
          ++auto_backend_requests;
          return zlink::framework::result_t<
            zlink::framework::runtime::messaging::message_parts_t>::success (parts);
      });
    auto auto_backend_reply =
      auto_backend_runtime
        .request_reply_parts (target_node, request_parts, std::chrono::milliseconds (25))
        .result ()
        .value ();
    if (!auto_backend_reply || auto_backend_reply.value ().size () != request_parts.size ()
        || auto_backend_requests != 1 || auto_backend_runtime.list_connections ().size () != 0) {
        return 383;
    }

    if (!route_runtime.complete_request (route_request.value ())
        || route_runtime.pending_request_count () != 0) {
        return 39;
    }
    route_runtime.stop ();
    if (route_runtime.running () || route_runtime.pending_request_count () != 0) {
        return 40;
    }

    return verify_route_dispatch (zlink, dispatch_errors, dispatch_errors_mutex, local_dispatch,
                                  provider, serializers, envelope_codec, request_header,
                                  route_request_parts, observed_dispatch_errors, route_runtime,
                                  target_node, target_spot);
}

int verify_public_route_submission (
  zlink::framework::service_provider_t &provider,
  zlink::framework::serializer_registry_t &serializers,
  zlink::framework::runtime::messaging::envelope_codec_t &envelope_codec,
  zlink::framework::runtime::messaging::envelope_header_t &request_header,
  const zlink::routing_id_t &target_node,
  const std::string &target_spot)
{
    zlink::framework::zlink_builder_t public_route_builder =
      zlink::framework::test::runtime_failure_builder ();
    public_route_builder.route_channel ("public.route")
      .bind ("tcp://public-bind:7700")
      .connect ("tcp://public-peer:7701")
      .add_handler_group ("public")
      .add_request_handler<local_handler_t, request_t, reply_t> (
        "request", &local_handler_t::handle_route_request)
      .add_send_handler<local_handler_t, event_t> ("event", &local_handler_t::handle_route_send);
    const auto public_route_channels =
      zlink::framework::detail::channel_runtime_manager_t::configured_route_channel_ids (
        public_route_builder);
    if (public_route_channels.size () != 1 || public_route_channels[0] != "public.route") {
        return 56;
    }
    auto public_manager =
      zlink::framework::detail::channel_runtime_manager_t::from (public_route_builder);
    public_manager.initialize_route_channels (public_route_builder);
    auto &public_route = public_manager.get_route_channel ("public.route");
    if (!public_route.running () || public_route.list_connections ().size () != 2) {
        return 57;
    }
    const auto public_spot_resolver_owner = std::make_shared<test_spot_address_resolver_t> ();
    auto &public_spot_resolver = *public_spot_resolver_owner;
    public_spot_resolver.set (
      "target-spot", zlink::framework::runtime::spot_address_t{
                       "public.route", zlink::routing_id_t::from ("target-node"), "target-spot"});
    zlink::framework::detail::channel_runtime_t::from (public_route_builder.message_bus ())
      .bind_spot_address_resolver (public_spot_resolver_owner);
    auto public_route_client = public_route_builder.route_client (serializers);
    std::atomic_int send_backend_seen = 0;
    public_route.set_send_backend (
      [&send_backend_seen,
       &envelope_codec] (const zlink::routing_id_t &target, const std::optional<std::string> &spot,
                         const zlink::framework::runtime::messaging::message_parts_t &parts)
        -> zlink::framework::result_t<void> {
          auto header = envelope_codec.decode_header (parts);
          if (target.to_string () != "target-node" || spot || !header
              || header.value ().message_name != event_t::packet_name
              || header.value ().metadata.find ("trace-id") == header.value ().metadata.end ()
              || header.value ().metadata.at ("trace-id") != "trace-send") {
              return zlink::framework::result_t<void>::failure (
                zlink::framework::framework_error_kind_t::internal_failure,
                "route send backend received unexpected packet");
          }
          ++send_backend_seen;
          return zlink::framework::result_t<void>::success ();
      });
    public_route_client
      .send_to_node ("public.route", zlink::routing_id_t::from (std::string ("target-node")),
                     event_t{31})
      .metadata ("trace-id", "trace-send")
      .async ();
    const auto send_backend_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (1);
    while (send_backend_seen.load () != 1
           && std::chrono::steady_clock::now () < send_backend_deadline) {
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    if (public_route.outbound_packets ().size () != 1 || send_backend_seen.load () != 1) {
        return 58;
    }
    auto public_send_header =
      envelope_codec.decode_header (public_route.outbound_packets ()[0].parts);
    if (!public_send_header
        || public_send_header.value ().kind
             != zlink::framework::runtime::messaging::message_kind_t::command
        || public_send_header.value ().channel_name != "public.route"
        || public_send_header.value ().message_name != event_t::packet_name
        || public_send_header.value ().metadata.at ("trace-id") != "trace-send"
        || public_route.outbound_packets ()[0].target_node_rid.to_string () != "target-node") {
        return 59;
    }
    public_route.set_request_backend (
      [&envelope_codec,
       &serializers] (const zlink::routing_id_t &target, const std::optional<std::string> &spot,
                      const zlink::framework::runtime::messaging::message_parts_t &parts,
                      std::chrono::milliseconds timeout)
        -> zlink::framework::result_t<zlink::framework::runtime::messaging::message_parts_t> {
          if (target.to_string () != "target-node" || spot
              || timeout != std::chrono::milliseconds (25)) {
              return zlink::framework::
                result_t<zlink::framework::runtime::messaging::message_parts_t>::failure (
                  zlink::framework::framework_error_kind_t::internal_failure,
                  "route request backend received unexpected target or timeout");
          }
          auto header = envelope_codec.decode_header (parts);
          auto body = envelope_codec.decode_body (parts);
          if (!header || !body || header.value ().message_name != request_t::packet_name
              || header.value ().metadata.find ("trace-id") == header.value ().metadata.end ()
              || header.value ().metadata.at ("trace-id") != "trace-request"
              || serializers.get<request_t> ()
                     .deserialize (
                       zlink::framework::detail::encoded_payload_from_raw (body.value ()))
                     .value
                   != 41) {
              return zlink::framework::
                result_t<zlink::framework::runtime::messaging::message_parts_t>::failure (
                  zlink::framework::framework_error_kind_t::internal_failure,
                  "route request backend received unexpected payload");
          }
          zlink::framework::runtime::messaging::envelope_header_t reply_header;
          reply_header.kind = zlink::framework::runtime::messaging::message_kind_t::response;
          reply_header.channel_name = "public.route";
          reply_header.message_name = header.value ().message_name;
          reply_header.content_type = header.value ().content_type;
          reply_t reply{1};
          return zlink::framework::result_t<zlink::framework::runtime::messaging::message_parts_t>::
            success (envelope_codec.encode_parts (reply_header, std::type_index (typeid (reply_t)),
                                                  &reply, serializers));
      });
    auto public_route_request =
      public_route_client
        .request_to_node ("public.route", zlink::routing_id_t::from (std::string ("target-node")),
                          request_t{41})
        .metadata ("trace-id", "trace-request")
        .timeout (std::chrono::milliseconds (25))
        .async<reply_t> ()
        .result ();
    if (!public_route_request || public_route_request.value ().value != 1
        || public_route.pending_request_count () != 0
        || public_route.outbound_packets ().size () != 2
        || !public_route.outbound_packets ()[1].request_seq) {
        return 60;
    }
    auto public_request_header =
      envelope_codec.decode_header (public_route.outbound_packets ()[1].parts);
    if (!public_request_header
        || public_request_header.value ().kind
             != zlink::framework::runtime::messaging::message_kind_t::request
        || public_request_header.value ().channel_name != "public.route"
        || public_request_header.value ().message_name != request_t::packet_name
        || public_request_header.value ().metadata.at ("trace-id") != "trace-request"
        || !public_request_header.value ().deadline) {
        return 61;
    }
    public_route.set_request_backend (
      [] (const zlink::routing_id_t &, const std::optional<std::string> &,
          const zlink::framework::runtime::messaging::message_parts_t &, std::chrono::milliseconds)
        -> zlink::framework::result_t<zlink::framework::runtime::messaging::message_parts_t> {
          return zlink::framework::result_t<zlink::framework::runtime::messaging::message_parts_t>::
            failure (zlink::framework::framework_error_kind_t::unavailable,
                     "route peer is not connected");
      });
    auto missing_peer_reply =
      public_route_client
        .request_to_node ("public.route", zlink::routing_id_t::from (std::string ("target-node")),
                          request_t{50})
        .timeout (std::chrono::milliseconds (10))
        .async<reply_t> ()
        .result ();
    // A failed admission ends the reply at once (request-admitted-stage); the route producer
    // removes its pending registration after that, so the registration is observed afterwards.
    const auto missing_peer_cleanup_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (1);
    while (public_route.pending_request_count () != 0
           && std::chrono::steady_clock::now () < missing_peer_cleanup_deadline) {
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    if (missing_peer_reply
        || missing_peer_reply.error_kind () != zlink::framework::framework_error_kind_t::unavailable
        || public_route.pending_request_count () != 0) {
        return 72;
    }
    public_route.set_request_backend (
      [&envelope_codec,
       &serializers] (const zlink::routing_id_t &target, const std::optional<std::string> &spot,
                      const zlink::framework::runtime::messaging::message_parts_t &parts,
                      std::chrono::milliseconds timeout)
        -> zlink::framework::result_t<zlink::framework::runtime::messaging::message_parts_t> {
          if (target.to_string () != "target-node" || spot
              || timeout != std::chrono::milliseconds (50)) {
              return zlink::framework::
                result_t<zlink::framework::runtime::messaging::message_parts_t>::failure (
                  zlink::framework::framework_error_kind_t::internal_failure,
                  "typed route backend received unexpected target or timeout");
          }
          auto header = envelope_codec.decode_header (parts);
          auto body = envelope_codec.decode_body (parts);
          if (!header || !body || header.value ().message_name != request_t::packet_name
              || header.value ().metadata.find ("trace-id") == header.value ().metadata.end ()
              || header.value ().metadata.at ("trace-id") != "trace-typed"
              || serializers.get<request_t> ()
                     .deserialize (
                       zlink::framework::detail::encoded_payload_from_raw (body.value ()))
                     .value
                   != 51) {
              return zlink::framework::
                result_t<zlink::framework::runtime::messaging::message_parts_t>::failure (
                  zlink::framework::framework_error_kind_t::internal_failure,
                  "typed route backend received unexpected payload");
          }
          zlink::framework::runtime::messaging::envelope_header_t reply_header;
          reply_header.kind = zlink::framework::runtime::messaging::message_kind_t::response;
          reply_header.channel_name = "public.route";
          reply_header.message_name = header.value ().message_name;
          reply_header.content_type = header.value ().content_type;
          reply_t reply{351};
          return zlink::framework::result_t<zlink::framework::runtime::messaging::message_parts_t>::
            success (envelope_codec.encode_parts (reply_header, std::type_index (typeid (reply_t)),
                                                  &reply, serializers));
      });
    auto public_typed_reply =
      public_route_client
        .request_to_node ("public.route", zlink::routing_id_t::from (std::string ("target-node")),
                          request_t{51})
        .metadata ("trace-id", "trace-typed")
        .timeout (std::chrono::milliseconds (50))
        .async<reply_t> ()
        .result ();
    if (!public_typed_reply || public_typed_reply.value ().value != 351
        || public_route.outbound_packets ().size () != 4
        || public_route.pending_request_count () != 0) {
        return 62;
    }

    public_route.set_request_backend (
      [&envelope_codec] (const zlink::routing_id_t &, const std::optional<std::string> &,
                         const zlink::framework::runtime::messaging::message_parts_t &parts,
                         std::chrono::milliseconds)
        -> zlink::framework::result_t<zlink::framework::runtime::messaging::message_parts_t> {
          const auto request_header = envelope_codec.decode_header (parts);
          if (!request_header) {
              return zlink::framework::
                result_t<zlink::framework::runtime::messaging::message_parts_t>::failure (
                  zlink::framework::framework_error_kind_t::internal_failure,
                  "route error regression received an invalid request");
          }
          zlink::framework::runtime::messaging::envelope_header_t error_header;
          error_header.kind = zlink::framework::runtime::messaging::message_kind_t::error;
          error_header.channel_name = "public.route";
          error_header.message_name = request_header.value ().message_name;
          error_header.correlation_id = request_header.value ().correlation_id;
          error_header.error_code = "not_found";
          error_header.error_message = "missing route handler";
          return zlink::framework::result_t<zlink::framework::runtime::messaging::message_parts_t>::
            success (
              envelope_codec.encode_raw_body_parts (error_header, zlink::message_t::from ("")));
      });
    const auto missing_route_handler =
      public_route_client
        .request_to_node ("public.route", zlink::routing_id_t::from (std::string ("target-node")),
                          request_t{52})
        .timeout (std::chrono::milliseconds (50))
        .async<reply_t> ()
        .result ();
    if (missing_route_handler
        || missing_route_handler.error_kind ()
             != zlink::framework::framework_error_kind_t::not_found
        || missing_route_handler.error () == nullptr
        || std::string (missing_route_handler.error ()->what ()) != "missing route handler") {
        return 78;
    }

    std::atomic_int spot_send_backend_seen = 0;
    public_route.set_send_backend (
      [&spot_send_backend_seen,
       &envelope_codec] (const zlink::routing_id_t &target, const std::optional<std::string> &spot,
                         const zlink::framework::runtime::messaging::message_parts_t &parts)
        -> zlink::framework::result_t<void> {
          auto header = envelope_codec.decode_header (parts);
          if (target.to_string () != "target-node" || !spot || *spot != "target-spot" || !header
              || header.value ().message_name != event_t::packet_name
              || header.value ().metadata.find ("trace-id") == header.value ().metadata.end ()
              || header.value ().metadata.at ("trace-id") != "trace-spot-send") {
              return zlink::framework::result_t<void>::failure (
                zlink::framework::framework_error_kind_t::internal_failure,
                "spot route send backend received unexpected packet");
          }
          ++spot_send_backend_seen;
          return zlink::framework::result_t<void>::success ();
      });
    public_route_client.send_to_spot ("target-spot", event_t{32})
      .metadata ("trace-id", "trace-spot-send")
      .async ();
    const auto spot_send_backend_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (1);
    while (spot_send_backend_seen.load () != 1
           && std::chrono::steady_clock::now () < spot_send_backend_deadline) {
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    if (spot_send_backend_seen.load () != 1) {
        return 76;
    }

    public_route.set_request_backend (
      [&envelope_codec,
       &serializers] (const zlink::routing_id_t &target, const std::optional<std::string> &spot,
                      const zlink::framework::runtime::messaging::message_parts_t &parts,
                      std::chrono::milliseconds timeout)
        -> zlink::framework::result_t<zlink::framework::runtime::messaging::message_parts_t> {
          if (target.to_string () != "target-node" || !spot || *spot != "target-spot"
              || timeout != std::chrono::milliseconds (50)) {
              return zlink::framework::
                result_t<zlink::framework::runtime::messaging::message_parts_t>::failure (
                  zlink::framework::framework_error_kind_t::internal_failure,
                  "spot route backend received unexpected target or timeout");
          }
          auto header = envelope_codec.decode_header (parts);
          auto body = envelope_codec.decode_body (parts);
          if (!header || !body || header.value ().message_name != request_t::packet_name
              || header.value ().metadata.find ("trace-id") == header.value ().metadata.end ()
              || header.value ().metadata.at ("trace-id") != "trace-spot-typed"
              || serializers.get<request_t> ()
                     .deserialize (
                       zlink::framework::detail::encoded_payload_from_raw (body.value ()))
                     .value
                   != 52) {
              return zlink::framework::
                result_t<zlink::framework::runtime::messaging::message_parts_t>::failure (
                  zlink::framework::framework_error_kind_t::internal_failure,
                  "spot route backend received unexpected payload");
          }
          zlink::framework::runtime::messaging::envelope_header_t reply_header;
          reply_header.kind = zlink::framework::runtime::messaging::message_kind_t::response;
          reply_header.channel_name = "public.route";
          reply_header.message_name = header.value ().message_name;
          reply_header.content_type = header.value ().content_type;
          reply_t reply{352};
          return zlink::framework::result_t<zlink::framework::runtime::messaging::message_parts_t>::
            success (envelope_codec.encode_parts (reply_header, std::type_index (typeid (reply_t)),
                                                  &reply, serializers));
      });
    auto public_spot_typed_reply =
      public_route_client.request_to_spot ("target-spot", request_t{52})
        .metadata ("trace-id", "trace-spot-typed")
        .timeout (std::chrono::milliseconds (50))
        .async<reply_t> ()
        .result ();
    if (!public_spot_typed_reply || public_spot_typed_reply.value ().value != 352) {
        return 77;
    }

    return verify_spot_routing (provider, serializers, envelope_codec, target_node, target_spot,
                                public_route, public_route_client);
}

int verify_live_route_submission (
  zlink::framework::service_provider_t &provider,
  zlink::framework::serializer_registry_t &serializers,
  zlink::framework::runtime::messaging::envelope_codec_t &envelope_codec,
  zlink::framework::detail::route_channel_runtime_t &public_route,
  zlink::framework::route_client_t &public_route_client)
{
    /* endpoint_connections live attach: post-apply connect/disconnect on the
     * public handle mutates the same bundle set requests iterate. */
    {
        zlink::framework::zlink_builder_t live_builder =
          zlink::framework::test::runtime_failure_builder ();
        live_builder.channel ("live.channel").enable_client ();
        auto live_runtime =
          zlink::framework::detail::channel_runtime_t::from (live_builder.message_bus ());
        zlink::framework::endpoint_connections_t handle;
        handle.connect ("tcp://127.0.0.1:19001");
        zlink::framework::detail::endpoint_connections_runtime_t::attach (
          handle,
          [&live_runtime] (const std::string &endpoint) {
              live_runtime.add_client_manual_connection ("live.channel", endpoint);
          },
          [&live_runtime] (const std::string &endpoint) {
              live_runtime.remove_client_manual_connection ("live.channel", endpoint);
          });
        // configured endpoint replayed on attach
        handle.connect ("tcp://127.0.0.1:19002");
        handle.disconnect ("tcp://127.0.0.1:19001");
        const auto listed = handle.list_connections ();
        if (listed != std::vector<std::string>{"tcp://127.0.0.1:19002"}) {
            return 146;
        }
        // subscriber role uses the same live-mutation seam
        live_builder.channel ("live.sub.channel").enable_subscriber ();
        zlink::framework::endpoint_connections_t subscriber_handle;
        subscriber_handle.connect ("tcp://127.0.0.1:19003");
        zlink::framework::detail::endpoint_connections_runtime_t::attach (
          subscriber_handle,
          [&live_runtime] (const std::string &endpoint) {
              live_runtime.add_subscriber_manual_connection ("live.sub.channel", endpoint);
          },
          [&live_runtime] (const std::string &endpoint) {
              live_runtime.remove_subscriber_manual_connection ("live.sub.channel", endpoint);
          });
        subscriber_handle.connect ("tcp://127.0.0.1:19004");
        subscriber_handle.disconnect ("tcp://127.0.0.1:19003");
        if (subscriber_handle.list_connections ()
            != std::vector<std::string>{"tcp://127.0.0.1:19004"}) {
            return 147;
        }
    }
    std::promise<void> delayed_backend_entered;
    zlink::framework::task_completion_source_t<
      zlink::framework::runtime::messaging::message_parts_t>
      delayed_backend_reply;
    auto delayed_backend_entered_future = delayed_backend_entered.get_future ();
    // The observer owns the flag: the task may complete after an early return.
    auto delayed_completed = std::make_shared<std::atomic_bool> (false);
    public_route.set_request_backend (
      [&delayed_backend_entered, &delayed_backend_reply] (
        const zlink::routing_id_t &, const std::optional<std::string> &,
        const zlink::framework::runtime::messaging::message_parts_t &, std::chrono::milliseconds) {
          delayed_backend_entered.set_value ();
          return zlink::framework::request_task_t<
            zlink::framework::runtime::messaging::message_parts_t> (
            delayed_backend_reply.task (),
            std::make_shared<zlink::framework::task_completion_source_t<void>> ());
      });
    auto delayed_task =
      public_route_client
        .request_to_node ("public.route", zlink::routing_id_t::from (std::string ("target-node")),
                          request_t{52})
        .timeout (std::chrono::milliseconds (50))
        .async<reply_t> ();
    zlink::framework::detail::observe_task_completion (
      delayed_task, [delayed_completed] (const zlink::framework::result_t<reply_t> &) {
          *delayed_completed = true;
      });
    if (delayed_backend_entered_future.wait_for (std::chrono::seconds (1))
        != std::future_status::ready) {
        return 63;
    }
    if (delayed_completed->load ()) {
        return 64;
    }
    zlink::framework::runtime::messaging::envelope_header_t delayed_reply_header;
    delayed_reply_header.kind = zlink::framework::runtime::messaging::message_kind_t::response;
    delayed_reply_header.channel_name = "public.route";
    delayed_reply_header.message_name = request_t::packet_name;
    reply_t delayed_reply_value{451};
    delayed_backend_reply.complete (
      zlink::framework::result_t<zlink::framework::runtime::messaging::message_parts_t>::success (
        envelope_codec.encode_parts (delayed_reply_header, std::type_index (typeid (reply_t)),
                                     &delayed_reply_value, serializers)));
    auto delayed_reply = delayed_task.result ();
    /* complete() publishes the result and then schedules the observers, so
     * result() can return before the observer ran. Wait for the observation
     * instead of reading it in the same breath. */
    const auto delayed_observed_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (1);
    while (!delayed_completed->load ()
           && std::chrono::steady_clock::now () < delayed_observed_deadline) {
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    if (!delayed_reply || delayed_reply.value ().value != 451 || !delayed_completed->load ()) {
        return 65;
    }

    return verify_actor_dispatch (provider, serializers, envelope_codec);
}

int verify_released_route_client (
  zlink::framework::serializer_registry_t &serializers,
  zlink::framework::runtime::messaging::envelope_codec_t &envelope_codec)
{
    // A released route client keeps the selected operation alive through physical completion.
    // Completion and subsequent runtime teardown must finish without waiting for one another.
    {
        auto released_builder = std::make_unique<zlink::framework::zlink_builder_t> (
          zlink::framework::test::runtime_failure_builder ());
        released_builder->route_channel ("released.route")
          .bind ("tcp://released-bind:7710")
          .connect ("tcp://released-peer:7711")
          .add_handler_group ("released")
          .add_request_handler<local_handler_t, request_t, reply_t> (
            "request", &local_handler_t::handle_route_request);
        auto released_manager =
          zlink::framework::detail::channel_runtime_manager_t::from (*released_builder);
        released_manager.initialize_route_channels (*released_builder);
        auto &released_route = released_manager.get_route_channel ("released.route");
        std::promise<void> released_backend_entered;
        zlink::framework::task_completion_source_t<
          zlink::framework::runtime::messaging::message_parts_t>
          released_backend_reply;
        zlink::framework::runtime::messaging::message_parts_t released_parts;
        released_route.set_request_backend (
          [&released_backend_entered, &released_backend_reply,
           &released_parts] (const zlink::routing_id_t &, const std::optional<std::string> &,
                             const zlink::framework::runtime::messaging::message_parts_t &parts,
                             std::chrono::milliseconds) {
              released_parts = parts;
              released_backend_entered.set_value ();
              return zlink::framework::request_task_t<
                zlink::framework::runtime::messaging::message_parts_t> (
                released_backend_reply.task (),
                std::make_shared<zlink::framework::task_completion_source_t<void>> ());
          });
        std::optional<zlink::framework::request_task_t<reply_t>> released_request;
        {
            auto released_client = released_builder->route_client (serializers);
            released_request.emplace (
              released_client
                .request_to_node ("released.route",
                                  zlink::routing_id_t::from (std::string ("target-node")),
                                  request_t{7})
                .timeout (std::chrono::milliseconds (2000))
                .async<reply_t> ());
        }
        if (released_backend_entered.get_future ().wait_for (std::chrono::seconds (5))
            != std::future_status::ready) {
            return 152;
        }
        auto released_header = envelope_codec.decode_header (released_parts);
        zlink::framework::runtime::messaging::envelope_header_t released_reply_header;
        released_reply_header.kind = zlink::framework::runtime::messaging::message_kind_t::response;
        released_reply_header.channel_name = "released.route";
        released_reply_header.message_name = released_header.value ().message_name;
        released_reply_header.content_type = released_header.value ().content_type;
        reply_t released_reply_value{7};
        released_backend_reply.complete (
          zlink::framework::result_t<zlink::framework::runtime::messaging::message_parts_t>::
            success (envelope_codec.encode_parts (released_reply_header,
                                                  std::type_index (typeid (reply_t)),
                                                  &released_reply_value, serializers)));
        auto released_reply = released_request->result ();
        released_request.reset ();
        if (!released_reply || released_reply.value ().value != 7) {
            return 153;
        }
        auto teardown =
          std::async (std::launch::async, [&released_builder] { released_builder.reset (); });
        if (teardown.wait_for (std::chrono::seconds (5)) != std::future_status::ready) {
            // Teardown did not finish; the process cannot unwind.
            std::_Exit (154);
        }
    }

    return 0;

    return 0;
}
} // namespace framework_channel_messaging_tests
