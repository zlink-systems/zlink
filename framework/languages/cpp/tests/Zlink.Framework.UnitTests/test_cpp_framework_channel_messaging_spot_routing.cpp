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

using namespace framework_channel_messaging_fixture;

namespace framework_channel_messaging_tests
{
int verify_spot_routing (zlink::framework::service_provider_t &provider,
                         zlink::framework::serializer_registry_t &serializers,
                         zlink::framework::runtime::messaging::envelope_codec_t &envelope_codec,
                         const zlink::routing_id_t &target_node,
                         const std::string &target_spot,
                         zlink::framework::detail::route_channel_runtime_t &public_route,
                         zlink::framework::route_client_t &public_route_client)
{
    zlink::framework::zlink_builder_t spot_only_builder =
      zlink::framework::test::runtime_failure_builder ();
    auto spot_only_runtime =
      zlink::framework::detail::channel_runtime_t::from (spot_only_builder.message_bus ());
    const auto spot_only_resolver_owner = std::make_shared<test_spot_address_resolver_t> ();
    auto &spot_only_resolver = *spot_only_resolver_owner;
    int spot_only_send_count = 0;
    int spot_only_node_send_count = 0;
    bool spot_only_request_called = false;
    /* The handle carries a concrete ObjectGeneration so the transport can assert the
     * exact generation the SPOT address snapshot published. */
    constexpr std::uint64_t spot_only_generation = 7;
    spot_only_resolver.set ("spot-rid", zlink::framework::runtime::spot_address_t{
                                          "spot-only", zlink::routing_id_t::from ("spot-node"),
                                          "spot-rid", spot_only_generation});
    spot_only_runtime.bind_spot_address_resolver (spot_only_resolver_owner);
    spot_only_runtime.bind_spot_mesh_transport (
      "spot-only",
      [&spot_only_send_count] (
        const zlink::routing_id_t &target_node_rid, const std::string &target_spot_id,
        std::uint64_t target_spot_generation, zlink::framework::runtime::messaging::message_parts_t)
        -> zlink::framework::task_t<zlink::framework::result_t<void>> {
          if (target_node_rid.to_string () == "spot-node" && target_spot_id == "spot-rid"
              && target_spot_generation == spot_only_generation) {
              ++spot_only_send_count;
          }
          co_return zlink::framework::result_t<void>::success ();
      },
      [&spot_only_request_called, &envelope_codec,
       &serializers] (const zlink::routing_id_t &target_node_rid, const std::string &target_spot_id,
                      std::uint64_t target_spot_generation,
                      zlink::framework::runtime::messaging::message_parts_t parts,
                      std::chrono::milliseconds timeout)
        -> zlink::framework::request_task_t<
          zlink::framework::result_t<zlink::framework::runtime::messaging::message_parts_t>> {
          const auto header = envelope_codec.decode_header (parts);
          const auto body = envelope_codec.decode_body (parts);
          if (!header || !body || target_node_rid.to_string () != "spot-node"
              || target_spot_id != "spot-rid" || target_spot_generation != spot_only_generation
              || timeout != std::chrono::milliseconds (75)
              || serializers.get<request_t> ()
                     .deserialize (
                       zlink::framework::detail::encoded_payload_from_raw (body.value ()))
                     .value
                   != 53) {
              co_return zlink::framework::
                result_t<zlink::framework::runtime::messaging::message_parts_t>::failure (
                  zlink::framework::framework_error_kind_t::internal_failure,
                  "spot-only transport received unexpected request");
          }
          spot_only_request_called = true;
          auto reply_header = header.value ();
          reply_header.kind = zlink::framework::runtime::messaging::message_kind_t::response;
          reply_t reply{353};
          co_await zlink::framework::detail::request_admitted_t{};
          co_return zlink::framework::
            result_t<zlink::framework::runtime::messaging::message_parts_t>::success (
              envelope_codec.encode_parts (reply_header, std::type_index (typeid (reply_t)), &reply,
                                           serializers));
      });
    /* Node direct and Spot messaging are separate transports in v11: common spec
     * 24-spot-address-messaging.ko.md §2 forbids deriving the owner node from the Spot ID
     * string, so a Node direct send no longer reaches the SPOT mesh sender carrying the
     * node RID as its Spot ID. Both sends below are still required to arrive; each is
     * checked against the transport its target kind routes through. */
    spot_only_runtime.bind_mesh_node_transport (
      "spot-only",
      [&spot_only_node_send_count] (const zlink::routing_id_t &target_node_rid,
                                    zlink::framework::runtime::messaging::message_parts_t)
        -> zlink::framework::task_t<zlink::framework::result_t<void>> {
          if (target_node_rid.to_string () == "spot-node") {
              ++spot_only_node_send_count;
          }
          co_return zlink::framework::result_t<void>::success ();
      },
      [] (const zlink::routing_id_t &, zlink::framework::runtime::messaging::message_parts_t,
          std::chrono::milliseconds, auto receiver) {
          using reply_t =
            zlink::framework::result_t<zlink::framework::runtime::messaging::message_parts_t>;
          receiver.observe (zlink::framework::request_task_t<reply_t> (
            zlink::framework::result_t<reply_t>::success (
              reply_t::failure (zlink::framework::framework_error_kind_t::internal_failure,
                                "spot-only node transport received an unexpected request"))));
      });
    auto spot_only_client = spot_only_builder.route_client (serializers);
    spot_only_client.send_to_spot ("spot-rid", event_t{33}).async ();
    spot_only_client
      .send_to_node ("spot-only", zlink::routing_id_t::from ("spot-node"), event_t{34})
      .async ();
    const auto spot_only_reply = spot_only_client.request_to_spot ("spot-rid", request_t{53})
                                   .timeout (std::chrono::milliseconds (75))
                                   .async<reply_t> ()
                                   .result ();
    if (spot_only_send_count != 1 || spot_only_node_send_count != 1 || !spot_only_reply
        || !spot_only_request_called || spot_only_reply.value ().value != 353) {
        return 142;
    }

    /* A Missing direct Spot is activated only when the fluent call carries
     * Instance intent. The resulting Ready address is used by the same
     * operation; a later call resolves that address and does not activate it
     * again. */
    zlink::framework::zlink_builder_t activation_builder =
      zlink::framework::test::runtime_failure_builder ();
    auto activation_runtime =
      zlink::framework::detail::channel_runtime_t::from (activation_builder.message_bus ());
    const auto activation_resolver_owner = std::make_shared<test_spot_address_resolver_t> ();
    auto &activation_resolver = *activation_resolver_owner;
    activation_runtime.bind_spot_address_resolver (activation_resolver_owner);
    std::atomic_int activation_count{0};
    std::atomic_int ready_instance_request_count{0};
    activation_runtime.bind_instance_spot_activator (
      [&] (const zlink::framework::spot_id_t &spot_id,
           const zlink::framework::detail::spot_activation_intent_t &intent,
           const std::optional<zlink::framework::runtime::spot_address_t> &cached_route,
           const std::string &, std::type_index, auto, const std::map<std::string, std::string> &,
           std::chrono::system_clock::time_point)
        -> zlink::framework::task_t<zlink::framework::result_t<void>> {
          if (std::string (spot_id) != "cart-17" || intent.mesh_name != "commerce"
              || intent.stable_type != "shopping-cart") {
              co_return zlink::framework::result_t<void>::failure (
                zlink::framework::framework_error_kind_t::not_configured,
                "unexpected Instance Spot activation intent");
          }
          ++activation_count;
          auto address = zlink::framework::runtime::spot_address_t{
            "commerce", zlink::routing_id_t::from ("cart-node"), "cart-17", 1};
          activation_resolver.set ("cart-17", address);
          co_return zlink::framework::result_t<void>::success ();
      },
      [&] (const auto &, const auto &, const auto &cached_route, auto, auto, auto, auto, auto) {
          if (!cached_route || cached_route->mesh_name != "commerce"
              || cached_route->node_rid.to_string () != "cart-node"
              || cached_route->spot_id != "cart-17") {
              return zlink::framework::request_task_t<zlink::message_t> (
                zlink::framework::result_t<zlink::message_t>::failure (
                  zlink::framework::framework_error_kind_t::internal_failure,
                  "Ready Instance route was not retained"));
          }
          ++ready_instance_request_count;
          return zlink::framework::request_task_t<zlink::message_t> (
            zlink::framework::result_t<zlink::message_t>::success (
              zlink::framework::detail::encoded_payload_to_raw (
                serializers.get<reply_t> ().serialize (reply_t{617}))));
      });
    std::atomic_int activation_send_count{0};
    std::atomic_int activation_request_count{0};
    activation_runtime.bind_spot_mesh_transport (
      "commerce",
      [&] (const zlink::routing_id_t &target_node, const std::string &target_spot,
           std::uint64_t generation, zlink::framework::runtime::messaging::message_parts_t)
        -> zlink::framework::task_t<zlink::framework::result_t<void>> {
          if (target_node.to_string () == "cart-node" && target_spot == "cart-17"
              && generation == 1) {
              ++activation_send_count;
          }
          co_return zlink::framework::result_t<void>::success ();
      },
      [&] (const zlink::routing_id_t &, const std::string &, std::uint64_t,
           zlink::framework::runtime::messaging::message_parts_t parts, std::chrono::milliseconds)
        -> zlink::framework::request_task_t<
          zlink::framework::result_t<zlink::framework::runtime::messaging::message_parts_t>> {
          ++activation_request_count;
          const auto header = envelope_codec.decode_header (parts);
          auto reply_header = header.value ();
          reply_header.kind = zlink::framework::runtime::messaging::message_kind_t::response;
          reply_t reply{617};
          co_await zlink::framework::detail::request_admitted_t{};
          co_return zlink::framework::
            result_t<zlink::framework::runtime::messaging::message_parts_t>::success (
              envelope_codec.encode_parts (reply_header, std::type_index (typeid (reply_t)), &reply,
                                           serializers));
      });
    auto activation_client = activation_builder.route_client (serializers);
    const auto activation_send = activation_client.send_to_spot ("cart-17", event_t{61})
                                   .instance_spot ("shopping-cart")
                                   .in_mesh ("commerce")
                                   .async ()
                                   .result ();
    const auto activation_reply = activation_client.request_to_spot ("cart-17", request_t{62})
                                    .instance_spot ("ignored-for-ready-owner")
                                    .in_mesh ("other-mesh")
                                    .async<reply_t> ()
                                    .result ();
    if (!activation_send || !activation_reply || activation_reply.value ().value != 617
        || activation_count.load () != 1 || activation_send_count.load () != 0
        || activation_request_count.load () != 0 || ready_instance_request_count.load () != 1) {
        return 150;
    }

    /* A stale resolved address fails the current operation without retry. The
     * next operation resolves the global SpotId again and may use a newer owner. */
    zlink::framework::zlink_builder_t retry_builder =
      zlink::framework::test::runtime_failure_builder ();
    auto retry_runtime =
      zlink::framework::detail::channel_runtime_t::from (retry_builder.message_bus ());
    const auto retry_resolver_owner = std::make_shared<test_spot_address_resolver_t> ();
    auto &retry_resolver = *retry_resolver_owner;
    retry_resolver.set ("moving-spot",
                        zlink::framework::runtime::spot_address_t{
                          "retry-mesh", zlink::routing_id_t::from ("retry-node"), "stale-spot"});
    retry_runtime.bind_spot_address_resolver (retry_resolver_owner);
    std::atomic_int retry_stale_attempts{0};
    std::atomic_int retry_fresh_attempts{0};
    retry_runtime.bind_spot_mesh_transport (
      "retry-mesh",
      [] (const zlink::routing_id_t &, const std::string &, std::uint64_t,
          zlink::framework::runtime::messaging::message_parts_t)
        -> zlink::framework::task_t<zlink::framework::result_t<void>> {
          co_return zlink::framework::result_t<void>::success ();
      },
      [&retry_stale_attempts, &retry_fresh_attempts, &envelope_codec, &serializers] (
        const zlink::routing_id_t &, const std::string &target_spot_id, std::uint64_t,
        zlink::framework::runtime::messaging::message_parts_t parts, std::chrono::milliseconds)
        -> zlink::framework::request_task_t<
          zlink::framework::result_t<zlink::framework::runtime::messaging::message_parts_t>> {
          if (target_spot_id == "stale-spot") {
              ++retry_stale_attempts;
              co_return zlink::framework::
                result_t<zlink::framework::runtime::messaging::message_parts_t>::failure (
                  zlink::framework::framework_error_kind_t::not_found,
                  "spot moved away from the stale address");
          }
          if (target_spot_id != "fresh-spot") {
              co_return zlink::framework::
                result_t<zlink::framework::runtime::messaging::message_parts_t>::failure (
                  zlink::framework::framework_error_kind_t::internal_failure,
                  "unexpected retry target");
          }
          ++retry_fresh_attempts;
          const auto header = envelope_codec.decode_header (parts);
          auto reply_header = header.value ();
          reply_header.kind = zlink::framework::runtime::messaging::message_kind_t::response;
          reply_t reply{454};
          co_await zlink::framework::detail::request_admitted_t{};
          co_return zlink::framework::
            result_t<zlink::framework::runtime::messaging::message_parts_t>::success (
              envelope_codec.encode_parts (reply_header, std::type_index (typeid (reply_t)), &reply,
                                           serializers));
      });
    auto retry_client = retry_builder.route_client (serializers);
    const auto stale_spot_reply = retry_client.request_to_spot ("moving-spot", request_t{54})
                                    .timeout (std::chrono::milliseconds (75))
                                    .async<reply_t> ()
                                    .result ();
    if (stale_spot_reply
        || stale_spot_reply.error_kind () != zlink::framework::framework_error_kind_t::not_found
        || retry_resolver.resolve_count.load () != 1 || retry_resolver.invalidate_count.load () != 1
        || retry_stale_attempts.load () != 1 || retry_fresh_attempts.load () != 0) {
        return 143;
    }
    retry_resolver.set ("moving-spot",
                        zlink::framework::runtime::spot_address_t{
                          "retry-mesh", zlink::routing_id_t::from ("retry-node"), "fresh-spot"});
    const auto fresh_reply = retry_client.request_to_spot ("moving-spot", request_t{54})
                               .timeout (std::chrono::milliseconds (75))
                               .async<reply_t> ()
                               .result ();
    if (!fresh_reply || fresh_reply.value ().value != 454
        || retry_resolver.resolve_count.load () != 2 || retry_stale_attempts.load () != 1
        || retry_fresh_attempts.load () != 1) {
        return 144;
    }

    return verify_live_route_submission (provider, serializers, envelope_codec, public_route,
                                         public_route_client);
}
} // namespace framework_channel_messaging_tests
