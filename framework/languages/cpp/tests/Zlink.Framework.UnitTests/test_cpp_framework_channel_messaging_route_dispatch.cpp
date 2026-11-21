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
#include "../support/channel_messaging_handler_fixture.hpp"
#include "../support/channel_messaging_dispatch_fixture.hpp"

using namespace framework_channel_messaging_fixture;

namespace
{
struct test_route_receive_result_t
{
    std::size_t dispatched = 0;
    std::vector<zlink::framework::detail::route_dispatch_reply_t> replies;
};

class test_route_receive_pump_t
{
  public:
    explicit test_route_receive_pump_t (
      zlink::framework::detail::route_packet_dispatcher_t dispatcher) :
        _dispatcher (std::move (dispatcher))
    {
    }

    void enqueue (zlink::framework::detail::route_received_packet_t packet)
    {
        _packets.push_back (std::move (packet));
    }

    zlink::framework::result_t<test_route_receive_result_t> drain ()
    {
        test_route_receive_result_t result;
        while (!_packets.empty ()) {
            auto packet = std::move (_packets.front ());
            _packets.pop_front ();
            auto dispatch = _dispatcher.dispatch (packet).result ();
            if (!dispatch) {
                return zlink::framework::detail::propagate_failure<test_route_receive_result_t> (
                  dispatch, "route packet dispatch failed");
            }
            ++result.dispatched;
            if (dispatch.value ().has_value ()) {
                result.replies.push_back (std::move (*dispatch.value ()));
            }
        }
        return zlink::framework::result_t<test_route_receive_result_t>::success (
          std::move (result));
    }

  private:
    zlink::framework::detail::route_packet_dispatcher_t _dispatcher;
    std::deque<zlink::framework::detail::route_received_packet_t> _packets;
};

class route_filter_t
{
  public:
    explicit route_filter_t (route_filter_state_t &state) : state (state) {}

    zlink::framework::task_t<void>
    invoke (const zlink::framework::handler_filter_context_t &context,
            zlink::framework::handler_next_t next)
    {
        state.seen_kinds.push_back (context.dispatch_kind);
        state.last_mesh = context.mesh_name.value_or ("<none>");
        state.last_channel = context.channel_name.value_or ("<none>");
        if ((state.reject_request
             && context.dispatch_kind
                  == zlink::framework::handler_dispatch_kind_t::node_direct_request)
            || (state.suppress_send
                && context.dispatch_kind
                     == zlink::framework::handler_dispatch_kind_t::node_direct_send)) {
            co_return;
        }
        co_await next ();
        co_return;
    }

    route_filter_state_t &state;
};
} // namespace

namespace
{
class local_internal_dispatcher_t final
    : public zlink::framework::detail::route_internal_packet_dispatcher_t
{
  public:
    bool can_handle_send (std::string_view packet_name) const override
    {
        return packet_name == "internal.send";
    }

    bool can_handle_request (std::string_view packet_name) const override
    {
        return packet_name == "internal.request";
    }

    zlink::framework::task_t<zlink::framework::result_t<void>>
    dispatch_send (const zlink::framework::detail::route_received_packet_t &received,
                   const zlink::framework::runtime::messaging::envelope_header_t &header,
                   zlink::framework::service_provider_t &services) const override
    {
        (void) received;
        (void) header;
        services.get_required<local_handler_state_t> ().internal_dispatch_provider_seen = 1;
        ++send_count;
        co_return zlink::framework::result_t<void>::success ();
    }

    zlink::framework::result_t<zlink::message_t>
    dispatch_request (const zlink::framework::detail::route_received_packet_t &received,
                      const zlink::framework::runtime::messaging::envelope_header_t &header,
                      zlink::framework::service_provider_t &services) const override
    {
        (void) received;
        services.get_required<local_handler_state_t> ().internal_dispatch_provider_seen = 2;
        if (header.message_name != "internal.request") {
            return zlink::framework::result_t<zlink::message_t>::failure (
              zlink::framework::framework_error_kind_t::not_found, "unsupported internal request");
        }
        return zlink::framework::result_t<zlink::message_t>::success (
          zlink::message_t::from (std::string ("88")));
    }

    mutable int send_count = 0;
};
} // namespace

namespace framework_channel_messaging_tests
{
int verify_route_dispatch (
  zlink::framework::zlink_builder_t &zlink,
  std::vector<zlink::framework::message_dispatch_error_event_t> &dispatch_errors,
  std::mutex &dispatch_errors_mutex,
  zlink::framework::dispatch_options_t &local_dispatch,
  zlink::framework::service_provider_t &provider,
  zlink::framework::serializer_registry_t &serializers,
  zlink::framework::runtime::messaging::envelope_codec_t &envelope_codec,
  zlink::framework::runtime::messaging::envelope_header_t &request_header,
  zlink::framework::runtime::messaging::message_parts_t &route_request_parts,
  std::vector<zlink::framework::message_dispatch_error_event_t> &observed_dispatch_errors,
  zlink::framework::detail::route_channel_runtime_t &route_runtime,
  const zlink::routing_id_t &target_node,
  const std::string &target_spot)
{
    zlink::framework::detail::channel_runtime_manager_t manager =
      zlink::framework::detail::channel_runtime_manager_t::from (zlink.message_bus ());
    manager.initialize_client_channels ();
    auto &client_bundle = manager.get_or_create_client_bundle ("profile");
    if (!client_bundle.contains_manual_connection ("tcp://127.0.0.1:7101")) {
        return 41;
    }
    manager.initialize_publisher_channels ();
    auto &publisher_bundle = manager.get_or_create_publisher_bundle ("events");
    if (!publisher_bundle.contains_manual_connection ("tcp://127.0.0.1:7201")) {
        return 42;
    }
    if (manager.monitoring_source ("profile.client") != "profile.client"
        || manager.monitoring_source ("events.publisher") != "events.publisher") {
        return 43;
    }
    bool missing_monitoring_failed = false;
    try {
        (void) manager.monitoring_source ("profile.server");
    }
    catch (const zlink::framework::framework_exception_t &error) {
        missing_monitoring_failed =
          error.kind () == zlink::framework::framework_error_kind_t::protocol_error;
    }
    if (!missing_monitoring_failed) {
        return 44;
    }
    manager.initialize_route_channels ({"profile"});
    auto &managed_route = manager.get_route_channel ("profile");
    if (!managed_route.running () || managed_route.list_connections ().empty ()) {
        return 45;
    }

    clear_dispatch_errors (dispatch_errors, dispatch_errors_mutex);
    zlink::framework::detail::route_handler_registry_t route_missing_handlers;
    zlink::framework::detail::no_route_internal_packet_dispatcher_t route_missing_internal;
    test_route_receive_pump_t route_pump{zlink::framework::detail::route_packet_dispatcher_t (
      "game.route", provider, serializers, route_missing_handlers, route_missing_internal,
      local_dispatch)};
    route_pump.enqueue (zlink::framework::detail::route_received_packet_t{
      zlink::routing_id_t::from (std::string ("source-node")), 77, route_request_parts});
    const auto route_receive = route_pump.drain ();
    if (!route_receive || route_receive.value ().dispatched != 1
        || route_receive.value ().replies.size () != 1
        || route_receive.value ().replies[0].request_seq.value_or (0) != 77) {
        return 46;
    }
    const auto route_error_header =
      envelope_codec.decode_header (route_receive.value ().replies[0].parts);
    if (!route_error_header
        || route_error_header.value ().kind
             != zlink::framework::runtime::messaging::message_kind_t::error
        || route_error_header.value ().channel_name != "game.route"
        || route_error_header.value ().error_code.value_or ("") != "not_found") {
        return 47;
    }
    observed_dispatch_errors = wait_dispatch_errors (dispatch_errors, dispatch_errors_mutex);
    if (observed_dispatch_errors.size () != 1
        || observed_dispatch_errors[0].surface
             != zlink::framework::dispatch_error_surface_t::route_mesh_channel
        || observed_dispatch_errors[0].message_kind
             != zlink::framework::dispatch_message_kind_t::request
        || observed_dispatch_errors[0].reason
             != zlink::framework::dispatch_error_reason_t::handler_missing
        || observed_dispatch_errors[0].action
             != zlink::framework::dispatch_error_action_t::reply_error
        || observed_dispatch_errors[0].packet_name.value_or ("") != "request"
        || observed_dispatch_errors[0].channel_name.value_or ("") != "game.route"
        || observed_dispatch_errors[0].topic.value_or ("") != "request"
        || observed_dispatch_errors[0].source_rid.value_or ("") != "source-node"
        || observed_dispatch_errors[0].correlation_id.value_or ("") != "corr-1") {
        return 98;
    }

    zlink::framework::detail::route_handler_registry_t route_handlers;
    route_handlers.on_request<local_handler_t, request_t, reply_t> (
      "game.route", "request", &local_handler_t::handle_route_request);
    route_handlers.on_send<local_handler_t, event_t> ("game.route", "event",
                                                      &local_handler_t::handle_route_send);
    zlink::framework::detail::no_route_internal_packet_dispatcher_t no_internal;
    const auto route_dispatch_thread = std::this_thread::get_id ();
    bool application_permit_released = false;
    provider.get_required<local_handler_state_t> ().application_permit_released =
      &application_permit_released;
    test_route_receive_pump_t route_handler_pump{
      zlink::framework::detail::route_packet_dispatcher_t (
        "game.route", provider, serializers, route_handlers, no_internal, {}, nullptr,
        zlink::framework::handler_dispatch_kind_t::node_direct_send,
        zlink::framework::handler_dispatch_kind_t::node_direct_request,
        [&] { application_permit_released = true; })};
    route_handler_pump.enqueue (zlink::framework::detail::route_received_packet_t{
      zlink::routing_id_t::from (std::string ("source-node")), 78, route_request_parts});
    const auto route_handler_receive = route_handler_pump.drain ();
    if (!route_handler_receive || route_handler_receive.value ().replies.size () != 1) {
        return 48;
    }
    const auto route_reply_body =
      envelope_codec.decode_body (route_handler_receive.value ().replies[0].parts);
    if (!route_reply_body
        || serializers.get<reply_t> ()
               .deserialize (
                 zlink::framework::detail::encoded_payload_from_raw (route_reply_body.value ()))
               .value
             != 224
        || provider.get_required<local_handler_state_t> ().last_route_request != 24
        || provider.get_required<local_handler_state_t> ().last_route_source != "source-node"
        || provider.get_required<local_handler_state_t> ().last_route_request_thread
             != route_dispatch_thread
        || !provider.get_required<local_handler_state_t> ().route_request_saw_permit_release) {
        return 49;
    }
    route_handler_pump.enqueue (zlink::framework::detail::route_received_packet_t{
      zlink::routing_id_t::from (std::string ("source-node")), std::nullopt,
      route_runtime.outbound_packets ()[0].parts});
    application_permit_released = false;
    const auto route_send_receive = route_handler_pump.drain ();
    if (!route_send_receive || !route_send_receive.value ().replies.empty ()
        || provider.get_required<local_handler_state_t> ().last_route_event != 78
        || provider.get_required<local_handler_state_t> ().last_route_send_thread
             != route_dispatch_thread
        || !provider.get_required<local_handler_state_t> ().route_send_saw_permit_release) {
        return 50;
    }

    zlink::framework::handler_registry_t route_filters;
    route_filters.use_filter<route_filter_t> ();
    auto &route_filter = provider.get_required<route_filter_state_t> ();
    route_filter.reject_request = true;
    provider.get_required<local_handler_state_t> ().last_route_request = 0;
    test_route_receive_pump_t filtered_route_pump{
      zlink::framework::detail::route_packet_dispatcher_t ("game.route", provider, serializers,
                                                           route_handlers, no_internal,
                                                           local_dispatch, &route_filters)};
    filtered_route_pump.enqueue (zlink::framework::detail::route_received_packet_t{
      zlink::routing_id_t::from (std::string ("source-node")), 79, route_request_parts});
    const auto filtered_request = filtered_route_pump.drain ();
    if (!filtered_request || filtered_request.value ().replies.size () != 1
        || provider.get_required<local_handler_state_t> ().last_route_request != 0
        || route_filter.seen_kinds.empty ()
        || route_filter.seen_kinds.back ()
             != zlink::framework::handler_dispatch_kind_t::node_direct_request
        || route_filter.last_mesh != "game.route" || route_filter.last_channel != "<none>") {
        return 409;
    }
    const auto filtered_error =
      envelope_codec.decode_header (filtered_request.value ().replies.front ().parts);
    if (!filtered_error
        || filtered_error.value ().kind
             != zlink::framework::runtime::messaging::message_kind_t::error
        || filtered_error.value ().error_code.value_or ("") != "rejected") {
        return 410;
    }

    route_filter.reject_request = false;
    route_filter.suppress_send = true;
    provider.get_required<local_handler_state_t> ().last_route_event = 0;
    filtered_route_pump.enqueue (zlink::framework::detail::route_received_packet_t{
      zlink::routing_id_t::from (std::string ("source-node")), std::nullopt,
      route_runtime.outbound_packets ()[0].parts});
    const auto filtered_send = filtered_route_pump.drain ();
    if (!filtered_send || !filtered_send.value ().replies.empty ()
        || provider.get_required<local_handler_state_t> ().last_route_event != 0
        || route_filter.seen_kinds.back ()
             != zlink::framework::handler_dispatch_kind_t::node_direct_send) {
        return 411;
    }

    route_filter.suppress_send = false;
    test_route_receive_pump_t filtered_channel_pump{
      zlink::framework::detail::route_packet_dispatcher_t (
        "game.route", provider, serializers, route_handlers, no_internal, local_dispatch,
        &route_filters, zlink::framework::handler_dispatch_kind_t::channel_send,
        zlink::framework::handler_dispatch_kind_t::channel_request)};
    filtered_channel_pump.enqueue (zlink::framework::detail::route_received_packet_t{
      zlink::routing_id_t::from (std::string ("source-node")), 80, route_request_parts});
    const auto filtered_channel_request = filtered_channel_pump.drain ();
    if (!filtered_channel_request || filtered_channel_request.value ().replies.size () != 1
        || route_filter.seen_kinds.back ()
             != zlink::framework::handler_dispatch_kind_t::channel_request) {
        return 412;
    }

    auto malformed_route_dispatch =
      zlink::framework::detail::route_packet_dispatcher_t ("game.route")
        .dispatch (zlink::framework::detail::route_received_packet_t{
          zlink::routing_id_t::from (std::string ("source-node")), std::nullopt,
          zlink::framework::runtime::messaging::message_parts_t (
            std::vector<zlink::message_t>{zlink::message_t::from (std::string ("not-json"))})})
        .result ();
    if (malformed_route_dispatch
        || malformed_route_dispatch.error_kind ()
             != zlink::framework::framework_error_kind_t::protocol_error) {
        return 401;
    }
    zlink::framework::runtime::messaging::envelope_header_t unsupported_route_header;
    unsupported_route_header.kind = zlink::framework::runtime::messaging::message_kind_t::response;
    unsupported_route_header.channel_name = "game.route";
    unsupported_route_header.message_name = "$zlink.unsupported";
    auto unsupported_route_parts = envelope_codec.encode_raw_body_parts (
      unsupported_route_header, zlink::message_t::from (std::string ("{}")));
    auto unsupported_route_dispatch =
      zlink::framework::detail::route_packet_dispatcher_t ("game.route", provider, serializers,
                                                           route_handlers, no_internal)
        .dispatch (zlink::framework::detail::route_received_packet_t{
          zlink::routing_id_t::from (std::string ("source-node")), std::nullopt,
          unsupported_route_parts})
        .result ();
    if (unsupported_route_dispatch
        || unsupported_route_dispatch.error_kind ()
             != zlink::framework::framework_error_kind_t::protocol_error) {
        return 402;
    }
    zlink::framework::runtime::messaging::envelope_header_t route_missing_body_header;
    route_missing_body_header.kind = zlink::framework::runtime::messaging::message_kind_t::command;
    route_missing_body_header.channel_name = "game.route";
    route_missing_body_header.message_name = "event";
    auto missing_body_header_only =
      zlink::framework::runtime::messaging::envelope_codec_t{}.encode_header (
        route_missing_body_header);
    auto missing_body_dispatch =
      zlink::framework::detail::route_packet_dispatcher_t ("game.route", provider, serializers,
                                                           route_handlers, no_internal)
        .dispatch (zlink::framework::detail::route_received_packet_t{
          zlink::routing_id_t::from (std::string ("source-node")), std::nullopt,
          zlink::framework::runtime::messaging::message_parts_t (
            std::vector<zlink::message_t>{missing_body_header_only})})
        .result ();
    if (missing_body_dispatch
        || missing_body_dispatch.error_kind ()
             != zlink::framework::framework_error_kind_t::protocol_error) {
        return 403;
    }

    local_internal_dispatcher_t internal_a;
    zlink::framework::runtime::messaging::envelope_header_t internal_header;
    internal_header.kind = zlink::framework::runtime::messaging::message_kind_t::request;
    internal_header.channel_name = "game.route";
    internal_header.message_name = "internal.request";
    internal_header.correlation_id = "internal-1";
    auto internal_parts = envelope_codec.encode_raw_body_parts (
      internal_header, zlink::message_t::from (std::string ("{}")));
    test_route_receive_pump_t internal_pump{zlink::framework::detail::route_packet_dispatcher_t (
      "game.route", provider, serializers, route_handlers, internal_a)};
    internal_pump.enqueue (zlink::framework::detail::route_received_packet_t{
      zlink::routing_id_t::from (std::string ("source-node")), 79, internal_parts});
    const auto internal_receive = internal_pump.drain ();
    if (!internal_receive || internal_receive.value ().replies.size () != 1) {
        return 51;
    }
    const auto internal_reply_body =
      envelope_codec.decode_body (internal_receive.value ().replies[0].parts);
    if (!internal_reply_body || internal_reply_body.value ().to_string () != "88") {
        return 52;
    }
    if (provider.get_required<local_handler_state_t> ().internal_dispatch_provider_seen != 2) {
        return 64;
    }
    if (no_internal.can_handle_send ("internal.send")
        || no_internal.can_handle_request ("internal.request")) {
        return 65;
    }
    const auto no_internal_send =
      no_internal
        .dispatch_send (
          zlink::framework::detail::route_received_packet_t{
            zlink::routing_id_t::from (std::string ("source-node")), std::nullopt, internal_parts},
          internal_header, provider)
        .result ()
        .value ();
    if (no_internal_send
        || no_internal_send.error_kind () != zlink::framework::framework_error_kind_t::not_found) {
        return 66;
    }
    const auto no_internal_request = no_internal.dispatch_request (
      zlink::framework::detail::route_received_packet_t{
        zlink::routing_id_t::from (std::string ("source-node")), 81, internal_parts},
      internal_header, provider);
    if (no_internal_request
        || no_internal_request.error_kind ()
             != zlink::framework::framework_error_kind_t::not_found) {
        return 67;
    }
    zlink::framework::runtime::messaging::envelope_header_t internal_send_header;
    internal_send_header.kind = zlink::framework::runtime::messaging::message_kind_t::command;
    internal_send_header.channel_name = "game.route";
    internal_send_header.message_name = "internal.send";
    auto internal_send_parts = envelope_codec.encode_raw_body_parts (
      internal_send_header, zlink::message_t::from (std::string ("{}")));
    const auto dispatch_internal_packet =
      [&] (const zlink::framework::detail::route_received_packet_t &received) {
          return zlink::framework::detail::route_packet_dispatcher_t (
                   "game.route", provider, serializers, route_handlers, internal_a)
            .dispatch (received)
            .result ();
      };
    const auto supported_internal_send =
      dispatch_internal_packet (zlink::framework::detail::route_received_packet_t{
        zlink::routing_id_t::from (std::string ("source-node")), std::nullopt,
        internal_send_parts});
    if (!internal_a.can_handle_send ("internal.send") || !supported_internal_send
        || supported_internal_send.value ().has_value () || internal_a.send_count != 1
        || provider.get_required<local_handler_state_t> ().internal_dispatch_provider_seen != 1) {
        return 68;
    }
    zlink::framework::runtime::messaging::envelope_header_t unsupported_internal_header;
    unsupported_internal_header.kind =
      zlink::framework::runtime::messaging::message_kind_t::command;
    unsupported_internal_header.channel_name = "game.route";
    unsupported_internal_header.message_name = "internal.unsupported";
    auto unsupported_internal_parts = envelope_codec.encode_raw_body_parts (
      unsupported_internal_header, zlink::message_t::from (std::string ("{}")));
    const auto unsupported_internal_send =
      dispatch_internal_packet (zlink::framework::detail::route_received_packet_t{
        zlink::routing_id_t::from (std::string ("source-node")), std::nullopt,
        unsupported_internal_parts});
    if (internal_a.can_handle_send ("internal.unsupported") || !unsupported_internal_send
        || unsupported_internal_send.value ().has_value () || internal_a.send_count != 1) {
        return 69;
    }
    unsupported_internal_header.kind =
      zlink::framework::runtime::messaging::message_kind_t::request;
    auto unsupported_internal_request_parts = envelope_codec.encode_raw_body_parts (
      unsupported_internal_header, zlink::message_t::from (std::string ("{}")));
    const auto unsupported_internal_request =
      dispatch_internal_packet (zlink::framework::detail::route_received_packet_t{
        zlink::routing_id_t::from (std::string ("source-node")), 82,
        unsupported_internal_request_parts});
    if (!unsupported_internal_request || !unsupported_internal_request.value ()) {
        return 70;
    }
    const auto unsupported_internal_request_header =
      envelope_codec.decode_header (unsupported_internal_request.value ()->parts);
    if (internal_a.can_handle_request ("internal.unsupported")
        || !unsupported_internal_request_header
        || unsupported_internal_request_header.value ().kind
             != zlink::framework::runtime::messaging::message_kind_t::error
        || unsupported_internal_request_header.value ().error_code
             != std::optional<std::string> ("not_found")) {
        return 70;
    }
    const auto invalid_internal_send =
      dispatch_internal_packet (zlink::framework::detail::route_received_packet_t{
        zlink::routing_id_t::from (std::string ("source-node")), std::nullopt,
        zlink::framework::runtime::messaging::message_parts_t (
          std::vector<zlink::message_t>{zlink::message_t::from (std::string ("not-json"))})});
    if (invalid_internal_send
        || invalid_internal_send.error_kind ()
             != zlink::framework::framework_error_kind_t::protocol_error) {
        return 71;
    }

    zlink::framework::detail::route_channel_registration_t route_registration ("registered.route");
    route_registration.bind ("tcp://registered-bind:7600")
      .connect ("tcp://registered-peer:7601")
      .add_handler_group ("game")
      .add_request_handler<local_handler_t, request_t, reply_t> (
        "request", &local_handler_t::handle_route_request)
      .add_send_handler<local_handler_t, event_t> ("event", &local_handler_t::handle_route_send);
    zlink::framework::detail::route_channel_initializer_t route_initializer;
    auto initialized_route = route_initializer.initialize (route_registration);
    if (!initialized_route.runtime->running ()
        || initialized_route.runtime->list_connections ().size () != 2
        || route_registration.handler_groups ().size () != 1
        || initialized_route.handlers.find (
             "registered.route", zlink::framework::runtime::messaging::message_kind_t::request,
             "request")
             == nullptr) {
        return 53;
    }
    zlink::framework::runtime::messaging::envelope_header_t registered_header;
    registered_header.kind = zlink::framework::runtime::messaging::message_kind_t::request;
    registered_header.channel_name = "registered.route";
    registered_header.message_name = "request";
    registered_header.correlation_id = "registered-1";
    auto registered_parts = envelope_codec.encode_raw_body_parts (
      registered_header, zlink::message_t::from (std::string ("25")));
    test_route_receive_pump_t registered_pump{zlink::framework::detail::route_packet_dispatcher_t (
      "registered.route", provider, serializers, initialized_route.handlers, no_internal)};
    registered_pump.enqueue (zlink::framework::detail::route_received_packet_t{
      zlink::routing_id_t::from (std::string ("source-node")), 80, registered_parts});
    auto registered_receive = registered_pump.drain ();
    if (!registered_receive || registered_receive.value ().replies.size () != 1) {
        return 54;
    }
    const auto registered_reply_body =
      envelope_codec.decode_body (registered_receive.value ().replies[0].parts);
    if (!registered_reply_body
        || serializers.get<reply_t> ()
               .deserialize (zlink::framework::detail::encoded_payload_from_raw (
                 registered_reply_body.value ()))
               .value
             != 225) {
        return 55;
    }

    return verify_public_route_submission (provider, serializers, envelope_codec, request_header,
                                           target_node, target_spot);
}
} // namespace framework_channel_messaging_tests
