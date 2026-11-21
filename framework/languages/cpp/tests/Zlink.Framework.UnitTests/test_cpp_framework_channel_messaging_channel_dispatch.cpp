/* SPDX-License-Identifier: Elastic-2.0 */
#include <zlink/json.hpp>
#include "../support/runtime_failure_fixture.hpp"
#include "../support/telemetry_observation.hpp"
#include <zlink/framework.hpp>
#include <zlink/codecs/protobuf.hpp>
#include "../support/read_text_file.hpp"
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
#include "../support/channel_messaging_dispatch_fixture.hpp"

using namespace framework_channel_messaging_fixture;

namespace
{
struct test_channel_receive_result_t
{
    std::size_t dispatched = 0;
    std::vector<zlink::framework::runtime::messaging::message_parts_t> replies;
};

class test_channel_receive_loop_t
{
  public:
    test_channel_receive_loop_t (zlink::framework::detail::channel_runtime_bundle_t &bundle,
                                 zlink::framework::detail::channel_packet_dispatcher_t dispatcher) :
        _bundle (bundle), _dispatcher (std::move (dispatcher))
    {
    }

    void enqueue_server_message (zlink::framework::runtime::messaging::message_parts_t parts)
    {
        _server_messages.push_back (std::move (parts));
    }

    zlink::framework::result_t<test_channel_receive_result_t>
    drain_server_messages (const std::string &channel_name,
                           zlink::framework::service_provider_t &services,
                           zlink::framework::serializer_registry_t &serializers,
                           const zlink::framework::handler_registry_t &handlers)
    {
        if (!_bundle.try_enter_receive ()) {
            return zlink::framework::result_t<test_channel_receive_result_t>::failure (
              zlink::framework::framework_error_kind_t::rejected,
              "channel receive loop is already active");
        }
        struct receive_guard_t
        {
            zlink::framework::detail::channel_runtime_bundle_t &bundle;
            ~receive_guard_t () { bundle.leave_receive (); }
        } receive_guard{_bundle};

        test_channel_receive_result_t result;
        while (!_server_messages.empty ()) {
            auto parts = std::move (_server_messages.front ());
            _server_messages.pop_front ();
            auto reply = _dispatcher.dispatch_server_message (channel_name, parts, services,
                                                              serializers, handlers);
            if (!reply) {
                return zlink::framework::result_t<test_channel_receive_result_t>::failure (
                  reply.error_kind (), reply.error () ? reply.error ()->what ()
                                                      : "channel receive loop dispatch failed");
            }
            ++result.dispatched;
            if (reply.value ().size () != 0) {
                result.replies.push_back (reply.value ());
            }
        }
        return zlink::framework::result_t<test_channel_receive_result_t>::success (
          std::move (result));
    }

    std::size_t pending_message_count () const noexcept { return _server_messages.size (); }

  private:
    zlink::framework::detail::channel_runtime_bundle_t &_bundle;
    zlink::framework::detail::channel_packet_dispatcher_t _dispatcher;
    std::deque<zlink::framework::runtime::messaging::message_parts_t> _server_messages;
};

class throwing_handler_t
{
  public:
    reply_t handle_request (const request_t &)
    {
        throw zlink::framework::framework_exception_t (
          zlink::framework::framework_error_kind_t::internal_failure, "DERR-007 handler exception");
    }
};

// Shared with the observer so errors arriving after the test scope ended still
// land in live storage.
struct dispatch_error_log_t
{
    std::vector<zlink::framework::message_dispatch_error_event_t> events;
    std::mutex mutex;
};

class recording_dispatch_observer_t
{
  public:
    explicit recording_dispatch_observer_t (std::shared_ptr<dispatch_error_log_t> log,
                                            std::filesystem::path log_path = {}) :
        _log (std::move (log)), _log_path (std::move (log_path))
    {
    }

    void on_dispatch_error (const zlink::framework::message_dispatch_error_event_t &error)
    {
        std::lock_guard lock (_log->mutex);
        _log->events.push_back (error);
        if (!_log_path.empty ()) {
            std::ofstream log (_log_path, std::ios::app);
            log << "dispatch-error" << " surface=" << surface_name (error.surface)
                << " messageKind=" << message_kind_name (error.message_kind)
                << " reason=" << reason_name (error.reason)
                << " action=" << action_name (error.action)
                << " packetName=" << error.packet_name.value_or ("")
                << " channelName=" << error.channel_name.value_or ("")
                << " correlationId=" << error.correlation_id.value_or ("") << '\n';
        }
        throw std::runtime_error ("observer failed");
    }

  private:
    static const char *surface_name (zlink::framework::dispatch_error_surface_t surface)
    {
        switch (surface) {
            case zlink::framework::dispatch_error_surface_t::channel:
                return "channel";
            case zlink::framework::dispatch_error_surface_t::route_mesh_channel:
                return "route_mesh_channel";
            case zlink::framework::dispatch_error_surface_t::spot_route:
                return "spot_route";
            case zlink::framework::dispatch_error_surface_t::spot_actor:
                return "spot_actor";
            case zlink::framework::dispatch_error_surface_t::spot_subscription:
                return "spot_subscription";
            case zlink::framework::dispatch_error_surface_t::stream_session:
                return "stream_session";
            default:
                return "unknown";
        }
    }

    static const char *message_kind_name (zlink::framework::dispatch_message_kind_t kind)
    {
        switch (kind) {
            case zlink::framework::dispatch_message_kind_t::request:
                return "request";
            case zlink::framework::dispatch_message_kind_t::send:
                return "send";
            case zlink::framework::dispatch_message_kind_t::publish:
                return "publish";
            case zlink::framework::dispatch_message_kind_t::response:
                return "response";
            case zlink::framework::dispatch_message_kind_t::error:
                return "error";
            case zlink::framework::dispatch_message_kind_t::actor_request:
                return "actor_request";
            case zlink::framework::dispatch_message_kind_t::actor_send:
                return "actor_send";
            default:
                return "unknown";
        }
    }

    static const char *reason_name (zlink::framework::dispatch_error_reason_t reason)
    {
        switch (reason) {
            case zlink::framework::dispatch_error_reason_t::handler_missing:
                return "handler_missing";
            case zlink::framework::dispatch_error_reason_t::payload_decode_failed:
                return "payload_decode_failed";
            case zlink::framework::dispatch_error_reason_t::handler_exception:
                return "handler_exception";
            case zlink::framework::dispatch_error_reason_t::invalid_frame:
                return "invalid_frame";
            case zlink::framework::dispatch_error_reason_t::reply_path_missing:
                return "reply_path_missing";
            case zlink::framework::dispatch_error_reason_t::unexpected_reply:
                return "unexpected_reply";
            default:
                return "unknown";
        }
    }

    static const char *action_name (zlink::framework::dispatch_error_action_t action)
    {
        switch (action) {
            case zlink::framework::dispatch_error_action_t::reply_error:
                return "reply_error";
            case zlink::framework::dispatch_error_action_t::drop:
                return "drop";
            case zlink::framework::dispatch_error_action_t::fail_caller:
                return "fail_caller";
            default:
                return "unknown";
        }
    }

    std::shared_ptr<dispatch_error_log_t> _log;
    std::filesystem::path _log_path;
};
} // namespace

using zlink::framework::tests::read_text_file;

namespace
{
std::size_t count_occurrences (const std::string &text, const std::string &needle)
{
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = text.find (needle, offset)) != std::string::npos) {
        ++count;
        offset += needle.size ();
    }
    return count;
}

std::string unique_inproc_endpoint (const char *base)
{
    static std::atomic<unsigned> counter{0};
    std::ostringstream stream;
    stream << "inproc://" << (base ? base : "framework-channel") << "-"
           << counter.fetch_add (1, std::memory_order_relaxed);
    return stream.str ();
}

bool wait_for_monitor_event (zlink::socket_monitor_t &monitor,
                             uint64_t event_type,
                             std::chrono::milliseconds timeout)
{
    zlink::poller_t poller;
    poller.add (monitor, zlink::poll_event_flag_t::pollin, 1);

    const auto deadline = std::chrono::steady_clock::now () + timeout;
    while (std::chrono::steady_clock::now () < deadline) {
        const auto remaining = deadline - std::chrono::steady_clock::now ();
        const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds> (remaining);
        zlink::poll_event_t event;
        if (poller.wait (&event, 1, remaining_ms) != 1) {
            continue;
        }
        const std::optional<zlink::monitor_event_t> monitor_event =
          monitor.recv (zlink::recv_flags_t::dontwait);
        if (monitor_event && static_cast<uint64_t> (monitor_event->event) == event_type) {
            return true;
        }
    }
    return false;
}
} // namespace

namespace framework_channel_messaging_tests
{
int verify_dispatch_observer_setup (zlink::framework::zlink_builder_t &zlink,
                                    zlink::framework::zlink_builder_t &outbound_only,
                                    zlink::framework::detail::channel_runtime_t &outbound_runtime)
{
    zlink::framework::zlink_builder_t local_server =
      zlink::framework::test::runtime_failure_builder ();
    local_server.channel ("local").enable_server ().bind ("tcp://127.0.0.1:7401");
    const auto dispatch_error_log = std::make_shared<dispatch_error_log_t> ();
    auto &dispatch_errors = dispatch_error_log->events;
    auto &dispatch_errors_mutex = dispatch_error_log->mutex;
    const auto dispatch_log_path =
      std::filesystem::temp_directory_path ()
      / ("zlink-cpp-derr-009-"
         + std::to_string (std::chrono::steady_clock::now ().time_since_epoch ().count ())
         + ".log");
    std::filesystem::remove (dispatch_log_path);
    zlink::framework::dispatch_options_t local_dispatch;
    auto dispatch_observer =
      std::make_shared<recording_dispatch_observer_t> (dispatch_error_log, dispatch_log_path);
    zlink::framework::detail::dispatch_options_access_t::set_dispatch_error_observer_for_tests (
      local_dispatch,
      [dispatch_observer] (const zlink::framework::message_dispatch_error_event_t &event) {
          dispatch_observer->on_dispatch_error (event);
      });
    zlink::framework::detail::apply_dispatch_options (local_server, local_dispatch);
    const auto reported_before_no_observer =
      zlink::framework::detail::dispatch_error_reporter_t::reported ();
    zlink::framework::detail::dispatch_error_reporter_t ({}).report (
      zlink::framework::message_dispatch_error_event_t{
        zlink::framework::dispatch_error_surface_t::channel,
        zlink::framework::dispatch_message_kind_t::send,
        zlink::framework::dispatch_error_reason_t::handler_missing,
        zlink::framework::dispatch_error_action_t::drop, std::string ("NoObserver"),
        std::string ("local"), std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        nullptr});
    if (zlink::framework::detail::dispatch_error_reporter_t::reported ()
        != reported_before_no_observer) {
        return 102;
    }

    return verify_outbound_submission (zlink, outbound_only, outbound_runtime, local_server,
                                       dispatch_errors, dispatch_errors_mutex, dispatch_log_path,
                                       local_dispatch);
}

int verify_channel_dispatch (
  zlink::framework::zlink_builder_t &zlink,
  zlink::framework::detail::channel_runtime_t &outbound_runtime,
  zlink::framework::zlink_builder_t &local_server,
  std::vector<zlink::framework::message_dispatch_error_event_t> &dispatch_errors,
  std::mutex &dispatch_errors_mutex,
  const std::filesystem::path &dispatch_log_path,
  zlink::framework::dispatch_options_t &local_dispatch,
  zlink::framework::service_provider_t &provider,
  zlink::framework::serializer_registry_t &serializers,
  std::atomic_int &request_deserialize_count)
{
    zlink::framework::handler_registry_t handlers;
    handlers.on_request<local_handler_t, request_t, reply_t> (
      "local", "request", &local_handler_t::handle_request, {.packet_name = "request"});
    handlers.on_request<local_handler_t, request_t, reply_t> (
      "hosted", "request", &local_handler_t::handle_request, {.packet_name = "request"});
    handlers.on_request<local_handler_t, request_t, reply_t> (
      "hosted-manual", "request", &local_handler_t::handle_request, {.packet_name = "request"});
    handlers.on_request<throwing_handler_t, request_t, reply_t> (
      "local", "request", &throwing_handler_t::handle_request, {.packet_name = "throw"});
    handlers.on_request<local_handler_t, request_t, reply_t> (
      "local", "request", &local_handler_t::handle_request,
      {.packet_name = request_t::packet_name});
    handlers.on_request<local_handler_t, request_t, reply_t> (
      "hosted", "request", &local_handler_t::handle_request,
      {.packet_name = request_t::packet_name});
    handlers.on_request<local_handler_t, request_t, reply_t> (
      "hosted-manual", "request", &local_handler_t::handle_request,
      {.packet_name = request_t::packet_name});
    handlers.on_send<local_handler_t, event_t> ("local", "send", &local_handler_t::handle_send,
                                                {.packet_name = "event"});
    handlers.on_send<local_handler_t, event_t> ("local", "send", &local_handler_t::handle_send,
                                                {.packet_name = event_t::packet_name});
    handlers.on_send<local_handler_t, event_t> ("hosted", "send", &local_handler_t::handle_send,
                                                {.packet_name = event_t::packet_name});
    handlers.on_send<local_handler_t, event_t> ("hosted", "send", &local_handler_t::handle_send,
                                                {.packet_name = "event"});
    handlers.on_event<local_handler_t, event_t> ("local", &local_handler_t::handle_send,
                                                 {.packet_name = "event"});

    auto local_runtime =
      zlink::framework::detail::channel_runtime_t::from (local_server.message_bus ());
    auto local_reply =
      local_runtime.dispatch_request ("local", "request", "request", provider, serializers,
                                      handlers, zlink::message_t::from (std::string ("23")));
    if (!local_reply
        || serializers.get<reply_t> ()
               .deserialize (
                 zlink::framework::detail::encoded_payload_from_raw (local_reply.value ()))
               .value
             != 123) {
        return 9;
    }

    const auto async_local_reply =
      local_runtime
        .dispatch_request_async ("local", "request", "request", provider, serializers, handlers,
                                 zlink::message_t::from (std::string ("23")))
        .result ();
    if (!async_local_reply || !async_local_reply.value ()
        || serializers.get<reply_t> ()
               .deserialize (zlink::framework::detail::encoded_payload_from_raw (
                 async_local_reply.value ().value ()))
               .value
             != 123) {
        return 113;
    }
    const auto async_local_send =
      local_runtime
        .dispatch_send_async ("local", "send", "event", provider, serializers, handlers,
                              zlink::message_t::from (std::string ("31")), {})
        .result ();
    if (!async_local_send || provider.get_required<local_handler_state_t> ().last_event != 31) {
        return 114;
    }

    zlink::framework::runtime::messaging::envelope_codec_t envelope_codec;
    zlink::framework::runtime::messaging::envelope_header_t request_header;
    request_header.kind = zlink::framework::runtime::messaging::message_kind_t::request;
    request_header.channel_name = "local";
    request_header.message_name = "request";
    request_header.topic = "request";
    request_header.correlation_id = "corr-1";
    auto request_parts = envelope_codec.encode_raw_body_parts (
      request_header, zlink::message_t::from (std::string ("24")));
    auto route_dispatch_request_header = request_header;
    route_dispatch_request_header.channel_name = "game.route";
    auto route_request_parts = envelope_codec.encode_raw_body_parts (
      route_dispatch_request_header, zlink::message_t::from (std::string ("24")));
    zlink::framework::detail::channel_packet_dispatcher_t packet_dispatcher (local_runtime);
    request_deserialize_count.store (0, std::memory_order_relaxed);
    const auto packet_reply = packet_dispatcher.dispatch_server_message (
      "local", request_parts, provider, serializers, handlers);
    if (!packet_reply) {
        return 18;
    }
    const auto packet_reply_header = envelope_codec.decode_header (packet_reply.value ());
    const auto packet_reply_body = envelope_codec.decode_body (packet_reply.value ());
    if (!packet_reply_header
        || packet_reply_header.value ().kind
             != zlink::framework::runtime::messaging::message_kind_t::response
        || packet_reply_header.value ().correlation_id != "corr-1" || !packet_reply_body
        || serializers.get<reply_t> ()
               .deserialize (
                 zlink::framework::detail::encoded_payload_from_raw (packet_reply_body.value ()))
               .value
             != 124
        || request_deserialize_count.load (std::memory_order_relaxed) != 1) {
        return 19;
    }

    request_header.topic.reset ();
    request_header.correlation_id = "corr-no-topic";
    auto no_topic_request_parts = envelope_codec.encode_raw_body_parts (
      request_header, zlink::message_t::from (std::string ("25")));
    const auto no_topic_packet_reply = packet_dispatcher.dispatch_server_message (
      "local", no_topic_request_parts, provider, serializers, handlers);
    if (!no_topic_packet_reply) {
        return 76;
    }
    const auto no_topic_packet_reply_body =
      envelope_codec.decode_body (no_topic_packet_reply.value ());
    if (!no_topic_packet_reply_body
        || serializers.get<reply_t> ()
               .deserialize (zlink::framework::detail::encoded_payload_from_raw (
                 no_topic_packet_reply_body.value ()))
               .value
             != 125) {
        return 77;
    }

    request_header.topic = "request";
    request_header.correlation_id = "corr-1";
    request_header.message_name = "missing";
    auto missing_parts = envelope_codec.encode_raw_body_parts (
      request_header, zlink::message_t::from (std::string ("24")));
    request_deserialize_count.store (0, std::memory_order_relaxed);
    const auto packet_error = packet_dispatcher.dispatch_server_message (
      "local", missing_parts, provider, serializers, handlers);
    if (!packet_error) {
        return 20;
    }
    const auto packet_error_header = envelope_codec.decode_header (packet_error.value ());
    if (!packet_error_header
        || packet_error_header.value ().kind
             != zlink::framework::runtime::messaging::message_kind_t::error
        || packet_error_header.value ().error_code.value_or ("") != "not_found"
        || request_deserialize_count.load (std::memory_order_relaxed) != 0) {
        return 21;
    }
    auto observed_dispatch_errors = wait_dispatch_errors (dispatch_errors, dispatch_errors_mutex);
    if (observed_dispatch_errors.size () != 1
        || observed_dispatch_errors[0].surface
             != zlink::framework::dispatch_error_surface_t::channel
        || observed_dispatch_errors[0].message_kind
             != zlink::framework::dispatch_message_kind_t::request
        || observed_dispatch_errors[0].reason
             != zlink::framework::dispatch_error_reason_t::handler_missing
        || observed_dispatch_errors[0].action
             != zlink::framework::dispatch_error_action_t::reply_error
        || observed_dispatch_errors[0].packet_name.value_or ("") != "missing"
        || observed_dispatch_errors[0].channel_name.value_or ("") != "local"
        || observed_dispatch_errors[0].topic.value_or ("") != "request"
        || observed_dispatch_errors[0].correlation_id.value_or ("") != "corr-1") {
        return 97;
    }
    clear_dispatch_errors (dispatch_errors, dispatch_errors_mutex);

    zlink::framework::runtime::messaging::envelope_header_t missing_send_header;
    missing_send_header.kind = zlink::framework::runtime::messaging::message_kind_t::command;
    missing_send_header.channel_name = "local";
    missing_send_header.message_name = "missing-command";
    missing_send_header.topic = "command";
    missing_send_header.correlation_id = "corr-send-missing";
    auto missing_send_parts = envelope_codec.encode_raw_body_parts (
      missing_send_header, zlink::message_t::from (std::string ("33")));
    const auto missing_send_result = packet_dispatcher.dispatch_server_message (
      "local", missing_send_parts, provider, serializers, handlers);
    if (!missing_send_result || missing_send_result.value ().size () != 0) {
        return 103;
    }
    observed_dispatch_errors = wait_dispatch_errors (dispatch_errors, dispatch_errors_mutex);
    if (observed_dispatch_errors.size () != 1
        || observed_dispatch_errors[0].surface
             != zlink::framework::dispatch_error_surface_t::channel
        || observed_dispatch_errors[0].message_kind
             != zlink::framework::dispatch_message_kind_t::send
        || observed_dispatch_errors[0].reason
             != zlink::framework::dispatch_error_reason_t::handler_missing
        || observed_dispatch_errors[0].action != zlink::framework::dispatch_error_action_t::drop
        || observed_dispatch_errors[0].packet_name.value_or ("") != "missing-command"
        || observed_dispatch_errors[0].channel_name.value_or ("") != "local"
        || observed_dispatch_errors[0].topic.value_or ("") != "command"
        || observed_dispatch_errors[0].correlation_id.value_or ("") != "corr-send-missing") {
        return 104;
    }
    clear_dispatch_errors (dispatch_errors, dispatch_errors_mutex);

    zlink::framework::runtime::messaging::envelope_header_t publish_header;
    publish_header.kind = zlink::framework::runtime::messaging::message_kind_t::publish;
    publish_header.channel_name = "local";
    publish_header.message_name = "event";
    publish_header.topic = "publish";
    publish_header.correlation_id = "corr-publish";
    publish_header.content_type = serializers.get<event_t> ().content_type ();
    auto publish_parts = envelope_codec.encode_raw_body_parts (
      publish_header, zlink::message_t::from (std::string ("34")));
    const auto packet_publish_result = packet_dispatcher.dispatch_server_message (
      "local", publish_parts, provider, serializers, handlers);
    if (!packet_publish_result || packet_publish_result.value ().size () != 0
        || provider.get_required<local_handler_state_t> ().last_event != 34) {
        return 111;
    }

    publish_header.message_name = "missing-publish";
    publish_header.correlation_id = "corr-publish-missing";
    auto missing_publish_parts = envelope_codec.encode_raw_body_parts (
      publish_header, zlink::message_t::from (std::string ("35")));
    const auto missing_publish_result = packet_dispatcher.dispatch_server_message (
      "local", missing_publish_parts, provider, serializers, handlers);
    if (!missing_publish_result || missing_publish_result.value ().size () != 0) {
        return 112;
    }
    observed_dispatch_errors = wait_dispatch_errors (dispatch_errors, dispatch_errors_mutex);
    if (observed_dispatch_errors.size () != 1
        || observed_dispatch_errors[0].surface
             != zlink::framework::dispatch_error_surface_t::channel
        || observed_dispatch_errors[0].message_kind
             != zlink::framework::dispatch_message_kind_t::publish
        || observed_dispatch_errors[0].reason
             != zlink::framework::dispatch_error_reason_t::handler_missing
        || observed_dispatch_errors[0].action != zlink::framework::dispatch_error_action_t::drop
        || observed_dispatch_errors[0].packet_name.value_or ("") != "missing-publish"
        || observed_dispatch_errors[0].channel_name.value_or ("") != "local"
        || observed_dispatch_errors[0].topic.value_or ("") != "publish"
        || observed_dispatch_errors[0].correlation_id.value_or ("") != "corr-publish-missing") {
        return 113;
    }
    clear_dispatch_errors (dispatch_errors, dispatch_errors_mutex);

    zlink::framework::runtime::messaging::envelope_header_t malformed_header;
    malformed_header.kind = zlink::framework::runtime::messaging::message_kind_t::request;
    malformed_header.channel_name = "local";
    malformed_header.message_name = "request";
    malformed_header.topic = "request";
    malformed_header.correlation_id = "corr-payload-decode";
    const auto last_request_before_malformed =
      provider.get_required<local_handler_state_t> ().last_request;
    auto malformed_parts = envelope_codec.encode_raw_body_parts (
      malformed_header, zlink::message_t::from (std::string ("not-an-int")));
    request_deserialize_count.store (0, std::memory_order_relaxed);
    const auto malformed_reply = packet_dispatcher.dispatch_server_message (
      "local", malformed_parts, provider, serializers, handlers);
    if (!malformed_reply) {
        return 108;
    }
    const auto malformed_reply_header = envelope_codec.decode_header (malformed_reply.value ());
    if (!malformed_reply_header
        || malformed_reply_header.value ().kind
             != zlink::framework::runtime::messaging::message_kind_t::error
        || malformed_reply_header.value ().error_code.value_or ("") != "protocol_error"
        || provider.get_required<local_handler_state_t> ().last_request
             != last_request_before_malformed
        || request_deserialize_count.load (std::memory_order_relaxed) != 1) {
        return 109;
    }
    observed_dispatch_errors = wait_dispatch_errors (dispatch_errors, dispatch_errors_mutex);
    if (observed_dispatch_errors.size () != 1
        || observed_dispatch_errors[0].surface
             != zlink::framework::dispatch_error_surface_t::channel
        || observed_dispatch_errors[0].message_kind
             != zlink::framework::dispatch_message_kind_t::request
        || observed_dispatch_errors[0].reason
             != zlink::framework::dispatch_error_reason_t::payload_decode_failed
        || observed_dispatch_errors[0].action
             != zlink::framework::dispatch_error_action_t::reply_error
        || observed_dispatch_errors[0].packet_name.value_or ("") != "request"
        || observed_dispatch_errors[0].channel_name.value_or ("") != "local"
        || observed_dispatch_errors[0].topic.value_or ("") != "request"
        || observed_dispatch_errors[0].correlation_id.value_or ("") != "corr-payload-decode"
        || !observed_dispatch_errors[0].exception
        || observed_dispatch_errors[0].error_type.value_or ("").empty ()
        || observed_dispatch_errors[0].error_message.value_or ("").empty ()
        || observed_dispatch_errors[0].error_message
             != malformed_reply_header.value ().error_message) {
        return 110;
    }
    try {
        std::rethrow_exception (observed_dispatch_errors[0].exception);
    }
    catch (const zlink::framework::framework_exception_t &error) {
        if (error.kind () != zlink::framework::framework_error_kind_t::protocol_error
            || observed_dispatch_errors[0].error_type.value_or ("") != typeid (error).name ()
            || observed_dispatch_errors[0].error_message.value_or ("") != error.what ())
            return 110;
    }
    clear_dispatch_errors (dispatch_errors, dispatch_errors_mutex);

    zlink::framework::runtime::messaging::envelope_header_t throwing_header;
    throwing_header.kind = zlink::framework::runtime::messaging::message_kind_t::request;
    throwing_header.channel_name = "local";
    throwing_header.message_name = "throw";
    throwing_header.topic = "request";
    throwing_header.correlation_id = "corr-handler-exception";
    auto throwing_parts = envelope_codec.encode_raw_body_parts (
      throwing_header, zlink::message_t::from (std::string ("24")));
    request_deserialize_count.store (0, std::memory_order_relaxed);
    const auto throwing_reply = packet_dispatcher.dispatch_server_message (
      "local", throwing_parts, provider, serializers, handlers);
    if (!throwing_reply) {
        return 105;
    }
    const auto throwing_reply_header = envelope_codec.decode_header (throwing_reply.value ());
    if (!throwing_reply_header
        || throwing_reply_header.value ().kind
             != zlink::framework::runtime::messaging::message_kind_t::error
        || throwing_reply_header.value ().error_code.value_or ("") != "internal_failure"
        || throwing_reply_header.value ().error_message.value_or ("")
             != "DERR-007 handler exception"
        || request_deserialize_count.load (std::memory_order_relaxed) != 1) {
        return 106;
    }
    observed_dispatch_errors = wait_dispatch_errors (dispatch_errors, dispatch_errors_mutex);
    if (observed_dispatch_errors.size () != 1
        || observed_dispatch_errors[0].surface
             != zlink::framework::dispatch_error_surface_t::channel
        || observed_dispatch_errors[0].message_kind
             != zlink::framework::dispatch_message_kind_t::request
        || observed_dispatch_errors[0].reason
             != zlink::framework::dispatch_error_reason_t::handler_exception
        || observed_dispatch_errors[0].action
             != zlink::framework::dispatch_error_action_t::reply_error
        || observed_dispatch_errors[0].packet_name.value_or ("") != "throw"
        || observed_dispatch_errors[0].channel_name.value_or ("") != "local"
        || observed_dispatch_errors[0].topic.value_or ("") != "request"
        || observed_dispatch_errors[0].correlation_id.value_or ("") != "corr-handler-exception"
        || !observed_dispatch_errors[0].exception
        || observed_dispatch_errors[0].error_type.value_or ("").empty ()
        || observed_dispatch_errors[0].error_message.value_or ("")
             != "DERR-007 handler exception") {
        return 107;
    }
    try {
        std::rethrow_exception (observed_dispatch_errors[0].exception);
    }
    catch (const zlink::framework::framework_exception_t &error) {
        if (error.kind () != zlink::framework::framework_error_kind_t::internal_failure
            || observed_dispatch_errors[0].error_type.value_or ("") != typeid (error).name ()
            || observed_dispatch_errors[0].error_message.value_or ("") != error.what ())
            return 107;
    }
    clear_dispatch_errors (dispatch_errors, dispatch_errors_mutex);
    const auto dispatch_log_text = read_text_file (dispatch_log_path);
    std::filesystem::remove (dispatch_log_path);
    if (count_occurrences (dispatch_log_text, "dispatch-error") < 5
        || dispatch_log_text.find ("surface=channel") == std::string::npos
        || dispatch_log_text.find ("messageKind=request") == std::string::npos
        || dispatch_log_text.find ("messageKind=send") == std::string::npos
        || dispatch_log_text.find ("messageKind=publish") == std::string::npos
        || dispatch_log_text.find ("reason=handler_missing") == std::string::npos
        || dispatch_log_text.find ("reason=payload_decode_failed") == std::string::npos
        || dispatch_log_text.find ("reason=handler_exception") == std::string::npos
        || dispatch_log_text.find ("action=reply_error") == std::string::npos
        || dispatch_log_text.find ("action=drop") == std::string::npos
        || dispatch_log_text.find ("packetName=missing") == std::string::npos
        || dispatch_log_text.find ("packetName=missing-command") == std::string::npos
        || dispatch_log_text.find ("packetName=request") == std::string::npos
        || dispatch_log_text.find ("packetName=throw") == std::string::npos
        || dispatch_log_text.find ("channelName=local") == std::string::npos
        || dispatch_log_text.find ("correlationId=corr-payload-decode") == std::string::npos) {
        return 111;
    }

    zlink::context_t native_context;
    zlink::router_socket_t native_server (native_context);
    zlink::dealer_socket_t native_client (native_context);
    zlink::framework::test::completion_poller_driver_t native_client_completion_owner (
      native_client);
    zlink::socket_monitor_t native_server_monitor = native_server.monitor_open ();
    zlink::socket_monitor_t native_client_monitor = native_client.monitor_open ();
    const auto native_endpoint = unique_inproc_endpoint ("framework-channel-request");
    native_server.bind (native_endpoint);
    native_client.connect (native_endpoint);
    if (!wait_for_monitor_event (native_server_monitor,
                                 static_cast<uint64_t> (zlink::monitor_event::connection_ready),
                                 std::chrono::milliseconds (2000))
        || !wait_for_monitor_event (native_client_monitor,
                                    static_cast<uint64_t> (zlink::monitor_event::connection_ready),
                                    std::chrono::milliseconds (2000))) {
        return 75;
    }
    std::this_thread::sleep_for (std::chrono::milliseconds (50));

    zlink::framework::runtime::messaging::client_call_codec_t client_codec;
    auto native_header = client_codec.create_envelope (
      zlink::framework::runtime::messaging::message_kind_t::request, "local", "request",
      std::chrono::milliseconds (1000), std::string ("request"));
    auto native_request_parts =
      client_codec.encode_envelope_parts (native_header, request_t{26}, serializers);

    auto native_server_done = std::async (std::launch::async, [&] () -> int {
        zlink::received_t native_received;
        if (native_server.recv (native_received) != 0 || !native_received.reply_token ()
            || native_received.parts ().size () != native_request_parts.size ()) {
            return 76;
        }
        const auto native_dispatch_reply = packet_dispatcher.dispatch_server_message (
          "local", copy_message_parts (native_received.parts ()), provider, serializers, handlers);
        if (!native_dispatch_reply) {
            return 77;
        }
        if (native_dispatch_reply.value ().size () != 2) {
            return 78;
        }
        zlink::message_t reply_header =
          zlink::message_t::from (native_dispatch_reply.value ()[0].to_string ());
        zlink::message_t reply_body =
          zlink::message_t::from (native_dispatch_reply.value ()[1].to_string ());
        native_received.reply ().message (reply_header).message (reply_body).submit ();
        return 0;
    });

    zlink::message_t native_request_header =
      zlink::message_t::from (native_request_parts[0].to_string ());
    zlink::message_t native_request_body =
      zlink::message_t::from (native_request_parts[1].to_string ());
    auto native_client_future = native_client.request ()
                                  .message (native_request_header)
                                  .message (native_request_body)
                                  .timeout (std::chrono::milliseconds (2000))
                                  .async ()
                                  .reply;
    auto native_client_result = await_native_reply (std::move (native_client_future)).result ();
    const int native_server_result = native_server_done.get ();
    if (native_server_result != 0) {
        return native_server_result;
    }
    const auto native_client_reply = copy_message_parts (native_client_result.value ());
    const auto decoded_native_reply = client_codec.decode_envelope_reply<reply_t> (
      native_client_reply, serializers, "native channel reply was empty",
      "native channel reply decode failed", "native channel request");
    if (!decoded_native_reply || decoded_native_reply.value ().value != 126) {
        return 79;
    }

    return verify_hosted_dispatch (
      zlink, outbound_runtime, dispatch_errors, dispatch_errors_mutex, local_dispatch, provider,
      serializers, handlers, local_runtime, envelope_codec, request_header, request_parts,
      route_request_parts, packet_dispatcher, observed_dispatch_errors);
}

int verify_channel_receive (
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
    zlink::framework::detail::channel_reply_writer_t reply_writer;
    const zlink::framework::framework_exception_t reply_errors[] = {
      {zlink::framework::framework_error_kind_t::unavailable, "mapped error"},
      {zlink::framework::framework_error_kind_t::not_found, "mapped error"},
      {zlink::framework::framework_error_kind_t::not_found, "mapped error"},
      {zlink::framework::framework_error_kind_t::rejected, "mapped error"},
      {zlink::framework::framework_error_kind_t::protocol_error, "mapped error"},
      zlink::framework::detail::make_boundary_exception (
        zlink::framework::detail::boundary_error_t::timed_out, "mapped error"),
      zlink::framework::detail::make_boundary_exception (
        zlink::framework::detail::boundary_error_t::shutdown, "mapped error"),
      zlink::framework::detail::make_boundary_exception (
        zlink::framework::detail::boundary_error_t::disconnected, "mapped error"),
      zlink::framework::detail::make_boundary_exception (
        zlink::framework::detail::boundary_error_t::closed, "mapped error"),
      {zlink::framework::framework_error_kind_t::internal_failure, "mapped error"}};
    const std::string reply_error_codes[] = {
      "unavailable",       "not_found",     "not_found",   "rejected",    "protocol_error",
      "deadline_exceeded", "shutting_down", "unavailable", "unavailable", "internal_failure"};
    for (std::size_t index = 0; index < std::size (reply_errors); ++index) {
        const auto &reply_error = reply_errors[index];
        const auto error_header =
          reply_writer.create_error_header ("local", request_header, reply_error);
        if (error_header.kind != zlink::framework::runtime::messaging::message_kind_t::error
            || error_header.error_code.value_or ("") != reply_error_codes[index]
            || error_header.error_message.value_or ("") != "mapped error") {
            return 72;
        }
    }

    zlink::framework::runtime::messaging::message_parts_t missing_body_parts (
      std::vector<zlink::message_t>{envelope_codec.encode_header (request_header)});
    const auto missing_body_error = packet_dispatcher.dispatch_server_message (
      "local", missing_body_parts, provider, serializers, handlers);
    if (!missing_body_error) {
        return 63;
    }
    const auto missing_body_header = envelope_codec.decode_header (missing_body_error.value ());
    if (!missing_body_header
        || missing_body_header.value ().kind
             != zlink::framework::runtime::messaging::message_kind_t::error
        || missing_body_header.value ().correlation_id != "corr-1"
        || missing_body_header.value ().error_code.value_or ("") != "protocol_error") {
        return 64;
    }

    auto local_send =
      local_runtime.dispatch_send ("local", "send", "event", provider, serializers, handlers,
                                   zlink::message_t::from (std::string ("31")));
    if (!local_send || provider.get_required<local_handler_state_t> ().last_event != 31) {
        return 10;
    }

    auto not_server =
      local_runtime.dispatch_request ("profile", "request", "request", provider, serializers,
                                      handlers, zlink::message_t::from (std::string ("1")));
    if (not_server
        || not_server.error_kind () != zlink::framework::framework_error_kind_t::unavailable) {
        return 11;
    }

    auto reservation = outbound_runtime.reserve_outbound_request ("profile");
    if (!reservation || outbound_runtime.pending_count () != 1) {
        return 12;
    }
    auto unmatched_reply = outbound_runtime.complete_outbound_reply (reservation.value () + 1000);
    if (unmatched_reply
        || unmatched_reply.error_kind ()
             != zlink::framework::framework_error_kind_t::protocol_error) {
        return 13;
    }
    auto matched_reply = outbound_runtime.complete_outbound_reply (reservation.value ());
    if (!matched_reply || outbound_runtime.pending_count () != 0) {
        return 14;
    }

    zlink::framework::detail::channel_runtime_bundle_t bundle;
    if (!bundle.try_add_manual_connection ("tcp://127.0.0.1:7401")
        || bundle.try_add_manual_connection ("tcp://127.0.0.1:7401")
        || !bundle.contains_manual_connection ("tcp://127.0.0.1:7401")) {
        return 22;
    }
    bundle.try_add_manual_connection ("tcp://127.0.0.1:7400");
    const auto manual_connections = bundle.list_manual_connections ();
    if (manual_connections.size () != 2 || manual_connections[0] != "tcp://127.0.0.1:7400"
        || manual_connections[1] != "tcp://127.0.0.1:7401") {
        return 23;
    }
    const auto first_manual_connection = bundle.next_manual_connection ();
    const auto second_manual_connection = bundle.next_manual_connection ();
    const auto third_manual_connection = bundle.next_manual_connection ();
    if (!first_manual_connection || !second_manual_connection || !third_manual_connection
        || first_manual_connection.value () != "tcp://127.0.0.1:7400"
        || second_manual_connection.value () != "tcp://127.0.0.1:7401"
        || third_manual_connection.value () != "tcp://127.0.0.1:7400") {
        return 240;
    }

    bundle.remove_manual_connection ("tcp://127.0.0.1:7401");
    if (bundle.contains_manual_connection ("tcp://127.0.0.1:7401")) {
        return 24;
    }

    test_channel_receive_loop_t receive_loop (
      bundle, zlink::framework::detail::channel_packet_dispatcher_t (local_runtime));
    receive_loop.enqueue_server_message (request_parts);
    if (receive_loop.pending_message_count () != 1) {
        return 25;
    }
    const auto receive_result =
      receive_loop.drain_server_messages ("local", provider, serializers, handlers);
    if (!receive_result || receive_result.value ().dispatched != 1
        || receive_result.value ().replies.size () != 1
        || receive_loop.pending_message_count () != 0 || bundle.receive_active ()) {
        return 26;
    }
    const auto loop_reply_header =
      envelope_codec.decode_header (receive_result.value ().replies[0]);
    const auto loop_reply_body = envelope_codec.decode_body (receive_result.value ().replies[0]);
    if (!loop_reply_header
        || loop_reply_header.value ().kind
             != zlink::framework::runtime::messaging::message_kind_t::response
        || !loop_reply_body
        || serializers.get<reply_t> ()
               .deserialize (
                 zlink::framework::detail::encoded_payload_from_raw (loop_reply_body.value ()))
               .value
             != 124) {
        return 27;
    }
    if (!bundle.try_enter_receive ()) {
        return 28;
    }
    const auto reentrant_result =
      receive_loop.drain_server_messages ("local", provider, serializers, handlers);
    bundle.leave_receive ();
    if (reentrant_result
        || reentrant_result.error_kind () != zlink::framework::framework_error_kind_t::rejected) {
        return 29;
    }

    return verify_route_submission (zlink, dispatch_errors, dispatch_errors_mutex, local_dispatch,
                                    provider, serializers, envelope_codec, request_header,
                                    request_parts, route_request_parts, observed_dispatch_errors);
}
} // namespace framework_channel_messaging_tests
