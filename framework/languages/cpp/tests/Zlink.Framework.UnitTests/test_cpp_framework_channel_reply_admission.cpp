/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework.hpp>

#include "runtime/channels/channel_host_service.hpp"
#include "runtime/channels/channel_runtime.hpp"
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
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace
{
using namespace std::chrono_literals;

std::string read_file (const std::filesystem::path &path)
{
    std::ifstream input (path);
    std::ostringstream contents;
    contents << input.rdbuf ();
    return contents.str ();
}

std::size_t count_text (std::string_view value, std::string_view needle)
{
    std::size_t count = 0;
    for (std::size_t offset = 0;
         (offset = value.find (needle, offset)) != std::string_view::npos;
         offset += needle.size ()) {
        ++count;
    }
    return count;
}

TEST (ChannelHostReplyAdmissionContract,
      OwnsOneBindingSubmitTerminalWithoutFrameworkRetry)
{
    const std::filesystem::path root = ZLINK_FRAMEWORK_CPP_SOURCE_DIR;
    const std::string source = read_file (
      root / "framework/src/runtime/channels/channel_host_service.cpp");
    const auto owner = source.find (
      "void reply (completed_reply_t completed)");
    ASSERT_NE (std::string::npos, owner);
    const auto end = source.find ("void clear_replies", owner);
    ASSERT_NE (std::string::npos, end);
    const std::string_view admission (source.data () + owner, end - owner);

    EXPECT_EQ (1u, count_text (admission, ".submit ()"));
    EXPECT_EQ (std::string_view::npos, admission.find (".async ()"));
    EXPECT_EQ (std::string_view::npos, admission.find ("co_await"));
    EXPECT_NE (std::string_view::npos,
               admission.find ("std::move (completed)"));
    EXPECT_EQ (std::string_view::npos,
               admission.find ("observe_task_completion"));
    EXPECT_EQ (std::string_view::npos, admission.find ("_replies.push_front"));
    EXPECT_EQ (std::string_view::npos, admission.find ("_replies.push_back"));
}

struct request_t
{
    static constexpr const char *packet_name =
      "channel.reply.admission.request";
    int value = 0;
};

struct reply_t
{
    int value = 0;
};

class blocking_request_handler_t
{
  public:
    reply_t handle (const request_t &request)
    {
        std::unique_lock lock (_mutex);
        _entered = true;
        _changed.notify_all ();
        _changed.wait (lock, [this] { return _released; });
        _returned = true;
        _changed.notify_all ();
        return {request.value + 1};
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

    bool wait_until_returned (std::chrono::milliseconds timeout)
    {
        std::unique_lock lock (_mutex);
        return _changed.wait_for (lock, timeout, [this] { return _returned; });
    }

  private:
    std::mutex _mutex;
    std::condition_variable _changed;
    bool _entered = false;
    bool _released = false;
    bool _returned = false;
};

std::string unique_inproc_endpoint ()
{
    static std::atomic<unsigned> counter{0};
    std::ostringstream value;
    value << "inproc://framework-channel-reply-admission-"
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

template <typename Predicate>
bool wait_until (Predicate predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now () + timeout;
    while (std::chrono::steady_clock::now () < deadline) {
        if (predicate ())
            return true;
        std::this_thread::sleep_for (5ms);
    }
    return predicate ();
}

class runtime_cleanup_t
{
  public:
    runtime_cleanup_t (
      blocking_request_handler_t &handler,
      zlink::framework::runtime::channel_host_service_t &host,
      zlink::router_socket_t &source) :
        _handler (&handler), _host (&host), _source (&source)
    {
    }

    ~runtime_cleanup_t ()
    {
        _handler->release ();
        _host->stop ();
        try {
            if (_source->valid ())
                _source->close ();
        }
        catch (...) {
        }
    }

  private:
    blocking_request_handler_t *_handler;
    zlink::framework::runtime::channel_host_service_t *_host;
    zlink::router_socket_t *_source;
};

TEST (ChannelHostReplyAdmission,
      MissingRouteTerminatesOnceWithOrdinaryEnvelopeOwnership)
{
    auto context = std::make_shared<zlink::context_t> ();
    context->options ().auto_hwm_enabled (false);

    const std::string endpoint = unique_inproc_endpoint ();
    const auto server_rid = zlink::routing_id_t::from (
      "framework-channel-reply-server");
    const auto source_rid = zlink::routing_id_t::from (
      "framework-channel-reply-source");

    zlink::framework::zlink_builder_t builder;
    builder.channel ("reply-admission")
      .enable_server ()
      .set_routing_id (server_rid)
      .bind (endpoint);
    auto runtime = zlink::framework::detail::channel_runtime_t::from (
      builder.message_bus ());
    runtime.bind_core_context (context);

    zlink::framework::serializer_registry_t serializers;
    serializers.add<request_t> (
      [] (const request_t &request) {
          return zlink::framework::encoded_payload_t::from_string (
            std::to_string (request.value));
      },
      [] (const zlink::framework::encoded_payload_t &payload) {
          return request_t{std::stoi (payload.to_string ())};
      },
      "application/json");
    serializers.add<reply_t> (
      [] (const reply_t &reply) {
          return zlink::framework::encoded_payload_t::from_string (
            std::to_string (reply.value));
      },
      [] (const zlink::framework::encoded_payload_t &payload) {
          return reply_t{std::stoi (payload.to_string ())};
      },
      "application/x-zlink-test-channel-reply");
    runtime.bind_serializers (serializers);

    zlink::framework::service_collection_t services;
    services.add_singleton<blocking_request_handler_t> ();
    auto provider = services.build_provider ();
    auto &handler = provider.get_required<blocking_request_handler_t> ();
    zlink::framework::handler_registry_t handlers;
    handlers.on_request<blocking_request_handler_t, request_t, reply_t> (
      "reply-admission", "request", &blocking_request_handler_t::handle,
      {.packet_name = request_t::packet_name});

    zlink::framework::runtime::channel_host_service_t host (
      builder.message_bus (), runtime.channel_snapshots (), handlers,
      serializers);
    host.start (provider);

    zlink::router_socket_t source (*context);
    runtime_cleanup_t cleanup (handler, host, source);
    source.set_routing_id (source_rid);
    auto source_monitor = source.monitor_open (
      zlink::monitor_event::connection_ready
      | zlink::monitor_event::disconnected);
    source.connect (endpoint);
    ASSERT_TRUE (wait_for_monitor_event (
      source_monitor, zlink::monitor_event::connection_ready, 2s));

    zlink::framework::runtime::messaging::envelope_header_t header;
    header.kind =
      zlink::framework::runtime::messaging::message_kind_t::request;
    header.channel_name = "reply-admission";
    header.message_name = request_t::packet_name;
    header.topic = "request";
    header.correlation_id = "reply-admission-correlation";
    auto request_parts =
      zlink::framework::runtime::messaging::envelope_codec_t{}.encode_parts (
        header, request_t{41}, serializers);
    zlink::message_t request_header = request_parts[0];
    zlink::message_t request_body = request_parts[1];
    auto pending_request = source.request (server_rid)
                             .message (request_header)
                             .message (request_body)
                             .timeout (5s)
                             .async ();

    ASSERT_TRUE (handler.wait_until_entered (2s));
    EXPECT_EQ (0u,
               context->core_hwm_budget_snapshot ()
                 .outstanding_application_lease_count ());

    source.disconnect (endpoint);
    ASSERT_TRUE (wait_until (
      [&] {
          return context->core_hwm_budget_snapshot ()
                   .active_completion_directional_queue_count ()
                 == 0u;
      },
      2s));
    handler.release ();
    ASSERT_TRUE (handler.wait_until_returned (2s));

    EXPECT_EQ (0u,
               context->core_hwm_budget_snapshot ()
                 .outstanding_application_lease_count ());

    (void) pending_request;
}

} // namespace
