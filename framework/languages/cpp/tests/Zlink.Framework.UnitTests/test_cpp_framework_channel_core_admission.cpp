/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#include "../support/runtime_failure_fixture.hpp"

// ClientServer channel admission belongs to Core (#1087 FW02-FW04):
// - the client submits to Core and maps Core's typed terminal; it does not
//   reject or delay a call on its own monitor-derived readiness (FW02);
// - the binding typed result, not errno, classifies a failure (FW03);
// - weight 0 removes future selection only; a record the server already
//   received still runs (server 03-client-server-channel §4, §6) (FW04).

#include <zlink/framework.hpp>

#include "../support/read_text_file.hpp"
#include "test_completion_poller_driver.hpp"

#include "runtime/channels/channel_host_service.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/dispatch/application_job_queue.hpp"
#include "runtime/messaging/envelope_codec.hpp"

#include <zlink/Contracts/Core/context.hpp>
#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Eventing/events.hpp>
#include <zlink/Contracts/Eventing/monitor.hpp>
#include <zlink/Contracts/Eventing/poll_event.hpp>
#include <zlink/Contracts/Eventing/poller.hpp>
#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include <zlink/Contracts/Sockets/routed_socket_contracts.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{
using namespace std::chrono_literals;

struct request_t
{
    static constexpr const char *packet_name = "channel.core.admission.request";
    int value = 0;
};

struct reply_t
{
    int value = 0;
};

class gated_request_handler_t
{
  public:
    // Request 1 holds its application job until released; every other
    // request answers at once.
    reply_t handle (const request_t &request)
    {
        if (request.value == 1) {
            std::unique_lock lock (_mutex);
            _entered = true;
            _changed.notify_all ();
            _changed.wait (lock, [this] { return _released; });
        }
        return {request.value + 100};
    }

    bool wait_until_entered (std::chrono::milliseconds timeout)
    {
        std::unique_lock lock (_mutex);
        return _changed.wait_for (lock, timeout, [this] { return _entered; });
    }

    void release ()
    {
        std::lock_guard lock (_mutex);
        _released = true;
        _changed.notify_all ();
    }

  private:
    std::mutex _mutex;
    std::condition_variable _changed;
    bool _entered = false;
    bool _released = false;
};

std::string unique_inproc_endpoint ()
{
    static std::atomic<unsigned> counter{0};
    std::ostringstream value;
    value << "inproc://framework-channel-core-admission-"
          << counter.fetch_add (1, std::memory_order_relaxed);
    return value.str ();
}

bool wait_for_monitor_event (zlink::socket_monitor_t &monitor,
                             zlink::monitor_event expected,
                             std::chrono::milliseconds timeout)
{
    zlink::poller_t poller;
    poller.add (monitor, zlink::poll_event_flag_t::pollin, 1);
    const auto deadline = std::chrono::steady_clock::now () + timeout;
    while (std::chrono::steady_clock::now () < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds> (
          deadline - std::chrono::steady_clock::now ());
        zlink::poll_event_t ready;
        if (poller.wait (&ready, 1, remaining) != 1)
            continue;
        const auto event = monitor.recv (zlink::recv_flags_t::dontwait);
        if (event && event->event == expected)
            return true;
    }
    return false;
}

void add_serializers (zlink::framework::serializer_registry_t &serializers)
{
    serializers.add<request_t> (
      [] (const request_t &request) {
          return zlink::framework::encoded_payload_t::from_string (std::to_string (request.value));
      },
      [] (const zlink::framework::encoded_payload_t &payload) {
          return request_t{std::stoi (payload.to_string ())};
      },
      "application/json");
    serializers.add<reply_t> (
      [] (const reply_t &reply) {
          return zlink::framework::encoded_payload_t::from_string (std::to_string (reply.value));
      },
      [] (const zlink::framework::encoded_payload_t &payload) {
          return reply_t{std::stoi (payload.to_string ())};
      },
      "application/x-zlink-test-channel-core-admission-reply");
}

zlink::framework::task_t<std::vector<zlink::message_t>>
await_reply (zlink::async_result_t<std::vector<zlink::message_t>> pending)
{
    co_return co_await std::move (pending);
}

int decode_reply (zlink::framework::serializer_registry_t &serializers,
                  const std::vector<zlink::message_t> &parts)
{
    std::vector<zlink::message_t> copied;
    for (const auto &part : parts)
        copied.push_back (zlink::message_t::from (part.to_string ()));
    zlink::framework::runtime::messaging::message_parts_t message (std::move (copied));
    zlink::framework::runtime::messaging::envelope_codec_t codec;
    const auto body = codec.decode_body (message);
    if (!body)
        return -1;
    return serializers.get<reply_t> ()
      .deserialize (zlink::framework::detail::encoded_payload_from_raw (body.value ()))
      .value;
}

void set_server_weight (zlink::framework::message_bus_t bus, const std::string &channel, int weight)
{
    zlink::framework::channel_runtime_options_t options (bus);
    auto server = options.client_server_channel (channel).configure_server_socket ();
    server.peer_weight (zlink::peer_weight_t::value (static_cast<std::uint32_t> (weight)));
}

TEST (ChannelCoreAdmission, WeightZeroServerStillRunsRequestsItAlreadyReceived)
{
    auto context = std::make_shared<zlink::context_t> ();
    const std::string channel = "core-admission-weight";
    const std::string endpoint = unique_inproc_endpoint ();
    const auto server_rid = zlink::routing_id_t::from ("framework-core-admission-server");

    zlink::framework::zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    builder.channel (channel).enable_server ().set_routing_id (server_rid).bind (endpoint);
    auto runtime = zlink::framework::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_core_context (context);
    zlink::framework::serializer_registry_t serializers;
    add_serializers (serializers);
    runtime.bind_serializers (serializers);

    zlink::framework::service_collection_t services;
    services.add_singleton<gated_request_handler_t> ();
    auto provider = services.build_provider ();
    auto &handler = provider.get_required<gated_request_handler_t> ();
    zlink::framework::handler_registry_t handlers;
    handlers.on_request<gated_request_handler_t, request_t, reply_t> (
      channel, "request", &gated_request_handler_t::handle,
      {.packet_name = request_t::packet_name});

    // One application job at a time: while request 1 runs, the receive loop
    // cannot take the next record, so records 2 and 3 are already received
    // by the server socket when weight 0 is applied.
    auto jobs = std::make_shared<zlink::framework::runtime::application_job_queue_t> (
      zlink::framework::runtime::application_job_queue_configuration_t{
        zlink::framework::application_job_queue_profile_t::balanced, std::nullopt, 1, 1});
    zlink::framework::runtime::channel_host_service_t host (
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> (),
      builder.message_bus (), runtime.channel_snapshots (), handlers, serializers, {}, jobs);
    host.start (provider);

    zlink::router_socket_t source (*context);
    {
        zlink::framework::test::completion_poller_driver_t completion_owner (source);
        source.set_routing_id (zlink::routing_id_t::from ("framework-core-admission-source"));
        auto monitor = source.monitor_open (zlink::monitor_event::connection_ready);
        source.connect (endpoint);
        ASSERT_TRUE (wait_for_monitor_event (monitor, zlink::monitor_event::connection_ready, 2s));

        std::vector<zlink::async_result_t<std::vector<zlink::message_t>>> pending;
        const auto submit = [&] (int value) {
            zlink::framework::runtime::messaging::envelope_header_t header;
            header.kind = zlink::framework::runtime::messaging::message_kind_t::request;
            header.channel_name = channel;
            header.message_name = request_t::packet_name;
            header.topic = "request";
            header.correlation_id = "core-admission-" + std::to_string (value);
            auto parts = zlink::framework::runtime::messaging::envelope_codec_t{}.encode_parts (
              header, request_t{value}, serializers);
            zlink::message_t first = parts[0];
            zlink::message_t second = parts[1];
            pending.push_back (source.request (server_rid)
                                 .message (first)
                                 .message (second)
                                 .timeout (5s)
                                 .async ()
                                 .reply);
        };
        submit (1);
        ASSERT_TRUE (handler.wait_until_entered (2s));
        submit (2);
        submit (3);
        set_server_weight (builder.message_bus (), channel, 0);
        handler.release ();

        for (std::size_t index = 0; index < pending.size (); ++index) {
            const auto reply = await_reply (std::move (pending[index])).result ();
            EXPECT_TRUE (reply) << "request " << index + 1 << " did not complete";
            if (reply)
                EXPECT_EQ (static_cast<int> (index + 101),
                           decode_reply (serializers, reply.value ()));
        }
    }
    host.stop ();
    source.close ();
}

// Messaging hot path I1: with no application permit the server stops
// receiving, but its loop keeps its management work. A reply that a running
// handler completes is delivered while the next request waits for a permit.
TEST (ChannelCoreAdmission, ServerDeliversRepliesWhileItWaitsForAPermit)
{
    auto context = std::make_shared<zlink::context_t> ();
    const std::string channel = "core-admission-permit-wait";
    const std::string endpoint = unique_inproc_endpoint ();
    const auto server_rid = zlink::routing_id_t::from ("framework-permit-wait-server");

    zlink::framework::zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    builder.channel (channel).enable_server ().set_routing_id (server_rid).bind (endpoint);
    auto runtime = zlink::framework::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_core_context (context);
    zlink::framework::serializer_registry_t serializers;
    add_serializers (serializers);
    runtime.bind_serializers (serializers);

    zlink::framework::service_collection_t services;
    services.add_singleton<gated_request_handler_t> ();
    auto provider = services.build_provider ();
    auto &handler = provider.get_required<gated_request_handler_t> ();
    zlink::framework::handler_registry_t handlers;
    handlers.on_request<gated_request_handler_t, request_t, reply_t> (
      channel, "request", &gated_request_handler_t::handle,
      {.packet_name = request_t::packet_name});

    auto jobs = std::make_shared<zlink::framework::runtime::application_job_queue_t> (
      zlink::framework::runtime::application_job_queue_configuration_t{
        zlink::framework::application_job_queue_profile_t::balanced, std::nullopt, 1, 1});
    zlink::framework::runtime::channel_host_service_t host (
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> (),
      builder.message_bus (), runtime.channel_snapshots (), handlers, serializers, {}, jobs);
    host.start (provider);

    zlink::router_socket_t source (*context);
    {
        zlink::framework::test::completion_poller_driver_t completion_owner (source);
        source.set_routing_id (zlink::routing_id_t::from ("framework-permit-wait-source"));
        auto monitor = source.monitor_open (zlink::monitor_event::connection_ready);
        source.connect (endpoint);
        ASSERT_TRUE (wait_for_monitor_event (monitor, zlink::monitor_event::connection_ready, 2s));

        const auto submit = [&] (int value) {
            zlink::framework::runtime::messaging::envelope_header_t header;
            header.kind = zlink::framework::runtime::messaging::message_kind_t::request;
            header.channel_name = channel;
            header.message_name = request_t::packet_name;
            header.topic = "request";
            header.correlation_id = "permit-wait-" + std::to_string (value);
            auto parts = zlink::framework::runtime::messaging::envelope_codec_t{}.encode_parts (
              header, request_t{value}, serializers);
            zlink::message_t first = parts[0];
            zlink::message_t second = parts[1];
            return source.request (server_rid)
              .message (first)
              .message (second)
              .timeout (5s)
              .async ()
              .reply;
        };
        auto first = submit (1);
        ASSERT_TRUE (handler.wait_until_entered (2s));
        // Request 1 returned its permit at handler entry; the test now holds
        // the only permit, so request 2 waits for supply.
        auto held = jobs->try_reserve_supply ();
        ASSERT_TRUE (held.has_value ());
        auto second = submit (2);
        std::this_thread::sleep_for (100ms);
        handler.release ();

        auto first_reply = std::async (std::launch::async,
                                       [&] { return await_reply (std::move (first)).result (); });
        const bool delivered_while_waiting = first_reply.wait_for (2s) == std::future_status::ready;
        held.reset ();
        const auto reply_one = first_reply.get ();
        const auto reply_two = await_reply (std::move (second)).result ();
        EXPECT_TRUE (delivered_while_waiting) << "the reply waited for the next request's permit";
        ASSERT_TRUE (reply_one);
        EXPECT_EQ (101, decode_reply (serializers, reply_one.value ()));
        ASSERT_TRUE (reply_two);
        EXPECT_EQ (102, decode_reply (serializers, reply_two.value ()));
    }
    host.stop ();
    source.close ();
}

TEST (ChannelCoreAdmission, ClientRequestWaitsOnCoreAdmissionNotFrameworkReadiness)
{
    // The server appears only after the framework's former private readiness
    // window (5 s) but well inside the request deadline. Core admits the
    // request once the peer is ready; the framework adds no deadline of its own.
    auto context = std::make_shared<zlink::context_t> ();
    const std::string channel = "core-admission-late";

    const std::string endpoint = unique_inproc_endpoint ();

    zlink::framework::serializer_registry_t serializers;
    add_serializers (serializers);
    zlink::framework::service_collection_t services;
    services.add_singleton<gated_request_handler_t> ();
    auto provider = services.build_provider ();
    zlink::framework::handler_registry_t handlers;
    handlers.on_request<gated_request_handler_t, request_t, reply_t> (
      channel, "request", &gated_request_handler_t::handle,
      {.packet_name = request_t::packet_name});

    zlink::framework::zlink_builder_t client_builder =
      zlink::framework::test::runtime_failure_builder ();
    client_builder.channel (channel).enable_client ().send_timeout (10s).connect (endpoint);
    auto client_runtime =
      zlink::framework::detail::channel_runtime_t::from (client_builder.message_bus ());
    client_runtime.bind_core_context (context);
    client_runtime.bind_serializers (serializers);

    zlink::framework::zlink_builder_t server_builder =
      zlink::framework::test::runtime_failure_builder ();
    server_builder.channel (channel).enable_server ().bind (endpoint);
    auto server_runtime =
      zlink::framework::detail::channel_runtime_t::from (server_builder.message_bus ());
    server_runtime.bind_core_context (context);
    server_runtime.bind_serializers (serializers);
    zlink::framework::runtime::channel_host_service_t host (
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> (),
      server_builder.message_bus (), server_runtime.channel_snapshots (), handlers, serializers,
      {});
    std::thread late_server ([&] {
        std::this_thread::sleep_for (5500ms);
        host.start (provider);
    });

    const auto reply = client_builder.request_client (channel)
                         .request (request_t{7})
                         .timeout (10s)
                         .async<reply_t> ()
                         .result ();
    late_server.join ();
    host.stop ();
    ASSERT_TRUE (reply) << (reply.error () != nullptr ? reply.error ()->what () : "no error");
    EXPECT_EQ (107, reply.value ().value);
}

TEST (ChannelCoreAdmissionContract, ClientClassifiesByBindingTypedResultOnly)
{
    // Core errors §Result와 errno 대응: the result is the classification and
    // errno only details the same failure. The channel client hands every
    // binding error to the submit/request result mapper; no errno test may
    // turn Rejected (ECONNREFUSED) into Unavailable, and no monitor-derived
    // ready count may decide admission before Core does.
    const std::filesystem::path root = ZLINK_FRAMEWORK_CPP_SOURCE_DIR;
    const std::string source = zlink::framework::tests::read_text_file (
      root / "framework/src/runtime/channels/channel_outbound_exchange.cpp");
    ASSERT_FALSE (source.empty ());
    EXPECT_EQ (std::string::npos, source.find ("internal_errno ()"));
    EXPECT_EQ (std::string::npos, source.find ("ready_count"));
    EXPECT_EQ (std::string::npos, source.find ("wait_for_connection_ready"));
}

TEST (ChannelCoreAdmission, RequestTimeoutStartsAfterCoreAdmission)
{
    constexpr auto reply_budget = 50ms;
    constexpr auto admission_observation = 200ms;
    auto context = std::make_shared<zlink::context_t> ();
    const std::string channel = "core-admission-reply-budget";
    const auto endpoint = unique_inproc_endpoint ();
    zlink::framework::serializer_registry_t serializers;
    add_serializers (serializers);
    zlink::framework::service_collection_t services;
    services.add_singleton<gated_request_handler_t> ();
    auto provider = services.build_provider ();
    auto &handler = provider.get_required<gated_request_handler_t> ();
    zlink::framework::handler_registry_t handlers;
    handlers.on_request<gated_request_handler_t, request_t, reply_t> (
      channel, "request", &gated_request_handler_t::handle,
      {.packet_name = request_t::packet_name});
    zlink::framework::zlink_builder_t client = zlink::framework::test::runtime_failure_builder ();
    client.channel (channel).enable_client ().connect (endpoint);
    auto client_runtime = zlink::framework::detail::channel_runtime_t::from (client.message_bus ());
    client_runtime.bind_core_context (context);
    client_runtime.bind_serializers (serializers);
    zlink::framework::zlink_builder_t server = zlink::framework::test::runtime_failure_builder ();
    server.channel (channel).enable_server ().bind (endpoint);
    auto server_runtime = zlink::framework::detail::channel_runtime_t::from (server.message_bus ());
    server_runtime.bind_core_context (context);
    server_runtime.bind_serializers (serializers);
    zlink::framework::runtime::channel_host_service_t host (
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> (),
      server.message_bus (), server_runtime.channel_snapshots (), handlers, serializers, {});
    auto pending = client.request_client (channel)
                     .request (request_t{1})
                     .timeout (reply_budget)
                     .async<reply_t> ();
    // No server can admit the request yet. Waiting longer than the reply
    // budget must leave the public task pending without cancelling it.
    const auto before_admission = pending.result_for (admission_observation);
    host.start (provider);
    const bool entered = handler.wait_until_entered (2s);
    // The admitted handler holds its reply until the caller's timeout wins.
    // This observes the budget without assuming a successful 50 ms round trip.
    const auto reply = pending.result ();
    handler.release ();
    host.stop ();
    EXPECT_FALSE (before_admission.has_value ()) << "reply timeout ran before Core admission";
    EXPECT_TRUE (entered) << "Core admission did not deliver the request to the handler";
    EXPECT_FALSE (reply);
    EXPECT_EQ (zlink::framework::framework_error_kind_t::deadline_exceeded, reply.error_kind ());
}

TEST (ChannelCoreAdmission, MissingServerExpiresAtDefaultAdmissionTimeout)
{
    auto context = std::make_shared<zlink::context_t> ();
    zlink::framework::serializer_registry_t serializers;
    add_serializers (serializers);
    zlink::framework::zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    const std::string channel = "core-admission-missing";
    builder.channel (channel).enable_client ().connect (unique_inproc_endpoint ());
    auto runtime = zlink::framework::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_core_context (context);
    runtime.bind_serializers (serializers);
    const auto started = std::chrono::steady_clock::now ();
    const auto result = builder.request_client (channel)
                          .request (request_t{7})
                          .timeout (10s)
                          .async<reply_t> ()
                          .result ();
    const auto elapsed = std::chrono::steady_clock::now () - started;
    EXPECT_FALSE (result);
    EXPECT_EQ (zlink::framework::framework_error_kind_t::deadline_exceeded, result.error_kind ());
    EXPECT_GE (elapsed, 900ms);
    EXPECT_LT (elapsed, 3s);
}

TEST (ChannelCoreAdmission, SendAdmissionDefaultIsIndependentOfRequestTimeout)
{
    auto context = std::make_shared<zlink::context_t> ();
    zlink::framework::serializer_registry_t serializers;
    add_serializers (serializers);
    zlink::framework::zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    const std::string channel = "core-send-admission-missing";
    auto configured = builder.channel (channel);
    configured.default_request_timeout (10s);
    configured.enable_client ().connect (unique_inproc_endpoint ());
    auto runtime = zlink::framework::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_core_context (context);
    runtime.bind_serializers (serializers);
    const auto started = std::chrono::steady_clock::now ();
    const auto result = builder.message_bus ().send (channel, request_t{7}).async ().result ();
    const auto elapsed = std::chrono::steady_clock::now () - started;
    EXPECT_FALSE (result);
    EXPECT_EQ (zlink::framework::framework_error_kind_t::deadline_exceeded, result.error_kind ());
    EXPECT_GE (elapsed, 900ms);
    EXPECT_LT (elapsed, 3s);
}

TEST (ChannelCoreAdmission, SendTimeoutConfigurationRoundsUpAndRejectsInvalidValues)
{
    zlink::framework::zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    auto client = builder.channel ("send-timeout-values").enable_client ();
    EXPECT_FALSE (client.snapshot ().send_timeout);
    client.send_timeout (1us);
    ASSERT_TRUE (client.snapshot ().send_timeout);
    EXPECT_EQ (1ms, *client.snapshot ().send_timeout);
    client.send_timeout (1500us);
    EXPECT_EQ (2ms, *client.snapshot ().send_timeout);
    const auto maximum = (std::numeric_limits<int>::max) ();
    client.send_timeout (std::chrono::milliseconds (maximum));
    EXPECT_EQ (std::chrono::milliseconds (maximum), *client.snapshot ().send_timeout);
    EXPECT_THROW (client.send_timeout (0ms), std::invalid_argument);
    EXPECT_THROW (client.send_timeout (-1ms), std::invalid_argument);
    EXPECT_THROW (client.send_timeout (std::chrono::milliseconds (
                    static_cast<std::chrono::milliseconds::rep> (maximum) + 1)),
                  std::invalid_argument);
    EXPECT_THROW (client.send_timeout (std::chrono::duration<double, std::milli> (
                    (std::numeric_limits<double>::infinity) ())),
                  std::invalid_argument);
    EXPECT_THROW (client.send_timeout (std::chrono::duration<double, std::milli> (
                    (std::numeric_limits<double>::quiet_NaN) ())),
                  std::invalid_argument);
    zlink::framework::service_collection_t services;
    zlink::framework::handler_registry_t handlers;
    zlink::framework::serializer_registry_t serializers;
    zlink::framework::zlink_framework_options_t options (services, handlers, serializers, builder);
    auto configured_client = options.add_client_server_channel ("client-timeout-values").client ();
    EXPECT_NO_THROW (configured_client.set_send_timeout (1us));
    EXPECT_THROW (configured_client.set_send_timeout (0ms), std::invalid_argument);
    auto publisher = options.add_fanout_channel ("publisher-timeout-values");
    EXPECT_NO_THROW (publisher.set_send_timeout (1500us));
    EXPECT_THROW (publisher.set_send_timeout (-1ms), std::invalid_argument);
}

} // namespace
