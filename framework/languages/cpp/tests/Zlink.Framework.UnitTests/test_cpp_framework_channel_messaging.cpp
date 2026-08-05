/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework.hpp>

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
#include "runtime/actors/actor_route_internal_dispatcher.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/messaging/client_call_codec.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/spots/spot_route_packets.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/streams/stream_runtime.hpp"

#include <zlink/Contracts/Core/byte_count.hpp>
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
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace
{

class test_spot_address_resolver_t final
    : public zlink::framework::runtime::spot_address_resolver_t
{
  public:
    void set (std::string spot_id, zlink::framework::runtime::spot_address_t address)
    {
        _addresses[std::move (spot_id)] = std::move (address);
    }

    zlink::framework::task_t<std::optional<zlink::framework::runtime::spot_address_t>>
    resolve_spot_address (std::string, std::string spot_id) override
    {
        ++resolve_count;
        const auto found = _addresses.find (spot_id);
        co_return found == _addresses.end ()
                    ? std::nullopt
                    : std::optional<zlink::framework::runtime::spot_address_t> (found->second);
    }

    void invalidate_spot_address (std::string_view spot_id) override
    {
        ++invalidate_count;
        _addresses.erase (std::string (spot_id));
    }

    void invalidate_all_routes_after_store_recovery () override
    {
        _addresses.clear ();
    }

    std::atomic_int resolve_count{0};
    std::atomic_int invalidate_count{0};

  private:
    std::map<std::string, zlink::framework::runtime::spot_address_t> _addresses;
};

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
            auto dispatch = _dispatcher.dispatch (packet);
            if (!dispatch) {
                return zlink::framework::detail::propagate_failure<test_route_receive_result_t> (dispatch, "route packet dispatch failed");
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

struct request_t
{
    static constexpr const char *packet_name = "profile.lookup";
    int value{};
};

struct reply_t
{
    int value{};
};

struct event_t
{
    static constexpr const char *packet_name = "profile.changed.event";
    int value{};
};

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

struct api_hop_request_t
{
    static constexpr const char *packet_name = "api-hop";
    int value{};
};

struct missing_probe_request_t
{
    static constexpr const char *packet_name = "missing.request";
    int value{};
};

class local_handler_t
{
  public:
    reply_t handle_request (const request_t &request)
    {
        last_request = request.value;
        return {request.value + 100};
    }

    void handle_send (const event_t &event)
    {
        if (event.value == 300) {
            std::unique_lock lock (send_gate_mutex);
            blocking_send_entered = true;
            send_gate_changed.notify_all ();
            send_gate_changed.wait (
              lock, [this] { return release_blocking_send; });
        }
        last_event = event.value;
    }

    reply_t handle_route_request (const request_t &request,
                                  const zlink::framework::route_message_context_t &context)
    {
        last_route_request = request.value;
        last_route_source = context.source_node_rid.to_string ();
        return {request.value + 200};
    }

    void handle_route_send (const event_t &event,
                            const zlink::framework::route_message_context_t &context)
    {
        last_route_event = event.value;
        last_route_source = context.source_node_rid.to_string ();
    }

    int last_request = 0;
    int last_event = 0;
    int last_route_request = 0;
    int last_route_event = 0;
    int internal_dispatch_provider_seen = 0;
    std::string last_route_source;
    std::mutex send_gate_mutex;
    std::condition_variable send_gate_changed;
    bool blocking_send_entered = false;
    bool release_blocking_send = false;
};

class route_filter_t
{
  public:
    zlink::framework::task_t<void>
    invoke (const zlink::framework::handler_filter_context_t &context,
            zlink::framework::handler_next_t next)
    {
        seen_kinds.push_back (context.dispatch_kind);
        last_mesh = context.mesh_name.value_or ("<none>");
        last_channel = context.channel_name.value_or ("<none>");
        if ((reject_request
             && context.dispatch_kind
                  == zlink::framework::handler_dispatch_kind_t::
                    node_direct_request)
            || (suppress_send
                && context.dispatch_kind
                     == zlink::framework::handler_dispatch_kind_t::
                       node_direct_send)) {
            co_return;
        }
        co_await next ();
        co_return;
    }

    bool reject_request = false;
    bool suppress_send = false;
    std::vector<zlink::framework::handler_dispatch_kind_t> seen_kinds;
    std::string last_mesh;
    std::string last_channel;
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

class route_orchestrating_handler_t
{
  public:
    explicit route_orchestrating_handler_t (zlink::framework::route_client_t routes) :
        _routes (std::move (routes))
    {
    }

    zlink::framework::task_t<reply_t> handle_request (const request_t &request)
    {
        co_return co_await _routes
          .request_to_node ("bingo.play", zlink::routing_id_t::from (std::string ("2201")), request)
          .timeout (std::chrono::milliseconds (500))
          .submit<reply_t> ();
    }

  private:
    zlink::framework::route_client_t _routes;
};

class reentrant_play_route_handler_t
{
  public:
    explicit reentrant_play_route_handler_t (zlink::framework::route_client_t routes) :
        _routes (std::move (routes))
    {
    }

    zlink::framework::task_t<reply_t> handle_outer (const outer_route_request_t &request)
    {
        auto api_reply = co_await _routes
                           .request_to_node (
                             "bingo.play", zlink::routing_id_t::from (std::string ("3302")),
                             api_hop_request_t{request.value + 1})
                           .timeout (std::chrono::milliseconds (500))
                           .submit<reply_t> ();
        co_return reply_t{api_reply.value + 1};
    }

    reply_t handle_inner (const inner_route_request_t &request) { return {request.value + 200}; }

  private:
    zlink::framework::route_client_t _routes;
};

class reentrant_api_route_handler_t
{
  public:
    explicit reentrant_api_route_handler_t (zlink::framework::route_client_t routes) :
        _routes (std::move (routes))
    {
    }

    zlink::framework::task_t<reply_t> handle_api_hop (const api_hop_request_t &request)
    {
        auto play_reply =
          co_await _routes
            .request_to_node ("bingo.play", zlink::routing_id_t::from (std::string ("2201")),
                              inner_route_request_t{request.value + 1})
            .timeout (std::chrono::milliseconds (500))
            .submit<reply_t> ();
        co_return reply_t{play_reply.value + 1};
    }

  private:
    zlink::framework::route_client_t _routes;
};

class recording_dispatch_observer_t : public zlink::framework::message_flow_observer_t
{
  public:
    explicit recording_dispatch_observer_t (
      std::vector<zlink::framework::message_flow_event_t> &events,
      std::mutex &mutex,
      std::filesystem::path log_path = {}) :
        _events (&events), _mutex (&mutex), _log_path (std::move (log_path))
    {
    }

    void on_message_flow (const zlink::framework::message_flow_event_t &error) override
    {
        if (error.outcome != zlink::framework::message_flow_outcome_t::error) {
            return;
        }
        std::lock_guard lock (*_mutex);
        _events->push_back (error);
        if (!_log_path.empty ()) {
            std::ofstream log (_log_path, std::ios::app);
            log << "dispatch-error" << " surface=" << surface_name (error.surface)
                << " messageKind=" << message_kind_name (error.message_kind)
                << " reason=" << reason_name (*error.error_reason)
                << " action=" << action_name (*error.error_action)
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

    std::vector<zlink::framework::message_flow_event_t> *_events;
    std::mutex *_mutex;
    std::filesystem::path _log_path;
};

std::string read_file (const std::filesystem::path &path)
{
    std::ifstream input (path);
    std::stringstream buffer;
    buffer << input.rdbuf ();
    return buffer.str ();
}

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

std::vector<zlink::framework::message_flow_event_t>
wait_dispatch_errors (std::vector<zlink::framework::message_flow_event_t> &events,
                      std::mutex &mutex,
                      std::size_t expected)
{
    for (int attempt = 0; attempt < 100; ++attempt) {
        {
            std::lock_guard lock (mutex);
            if (events.size () >= expected) {
                return events;
            }
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    std::lock_guard lock (mutex);
    return events;
}

void clear_dispatch_errors (std::vector<zlink::framework::message_flow_event_t> &events,
                            std::mutex &mutex)
{
    std::lock_guard lock (mutex);
    events.clear ();
}

class nested_request_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<zlink::framework::channel_client_t>;

    explicit nested_request_handler_t (zlink::framework::channel_client_t &client) :
        _client (client)
    {
    }

    zlink::framework::task_t<reply_t> handle_request (const request_t &request)
    {
        if (request.value == 50) {
            auto nested = co_await _client.request ("hosted-nested", request_t{51})
                            .timeout (std::chrono::milliseconds (2000))
                            .submit<reply_t> ();
            co_return reply_t{nested.value + 1};
        }
        co_return reply_t{request.value + 100};
    }

  private:
    zlink::framework::channel_client_t &_client;
};

struct scoped_channel_dependency_t
{
    scoped_channel_dependency_t () { ++created; }
    ~scoped_channel_dependency_t () { ++destroyed; }

    inline static std::atomic_int created{0};
    inline static std::atomic_int destroyed{0};
    int offset = 300;
};

class scoped_channel_filter_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<scoped_channel_dependency_t>;

    explicit scoped_channel_filter_t (
      scoped_channel_dependency_t &dependency) :
        _dependency (dependency)
    {
    }

    zlink::framework::task_t<void>
    invoke (const zlink::framework::handler_filter_context_t &,
            zlink::framework::handler_next_t next)
    {
        _dependency.offset = 400;
        co_await next ();
        co_return;
    }

  private:
    scoped_channel_dependency_t &_dependency;
};

class scoped_channel_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<scoped_channel_dependency_t>;

    explicit scoped_channel_handler_t (scoped_channel_dependency_t &dependency) :
        _dependency (dependency)
    {
    }

    reply_t handle_request (const request_t &request)
    {
        return {request.value + _dependency.offset};
    }

  private:
    scoped_channel_dependency_t &_dependency;
};

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

    zlink::framework::result_t<void>
    dispatch_send (const zlink::framework::detail::route_received_packet_t &received,
                   zlink::framework::service_provider_t &services) const override
    {
        (void) received;
        services.get_required<local_handler_t> ().internal_dispatch_provider_seen = 1;
        ++send_count;
        return zlink::framework::result_t<void>::success ();
    }

    zlink::framework::result_t<zlink::message_t>
    dispatch_request (const zlink::framework::detail::route_received_packet_t &received,
                      const zlink::framework::runtime::messaging::envelope_header_t &header,
                      zlink::framework::service_provider_t &services) const override
    {
        (void) received;
        services.get_required<local_handler_t> ().internal_dispatch_provider_seen = 2;
        if (header.message_name != "internal.request") {
            return zlink::framework::result_t<zlink::message_t>::failure (
              zlink::framework::framework_error_kind_t::not_found,
              "unsupported internal request");
        }
        return zlink::framework::result_t<zlink::message_t>::success (
          zlink::message_t::from (std::string ("88")));
    }

    mutable int send_count = 0;
};

template <typename T> void add_int_serializer (zlink::framework::serializer_registry_t &serializers)
{
    serializers.add<T> (
      [] (const T &value) {
          return zlink::framework::encoded_payload_t::from_string (std::to_string (value.value));
      },
      [] (const zlink::framework::encoded_payload_t &payload) {
          return T{std::stoi (payload.to_string ())};
      });
}

std::string unique_inproc_endpoint (const char *base)
{
    static std::atomic<unsigned> counter{0};
    std::ostringstream stream;
    stream << "inproc://" << (base ? base : "framework-channel") << "-"
           << counter.fetch_add (1, std::memory_order_relaxed);
    return stream.str ();
}

std::string unique_tcp_endpoint ()
{
    const int fd = ::socket (AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error ("failed to create TCP probe socket");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind (fd, reinterpret_cast<sockaddr *> (&address), sizeof (address)) != 0) {
        ::close (fd);
        throw std::runtime_error ("failed to bind TCP probe socket");
    }
    socklen_t length = sizeof (address);
    if (::getsockname (fd, reinterpret_cast<sockaddr *> (&address), &length) != 0) {
        ::close (fd);
        throw std::runtime_error ("failed to read TCP probe socket port");
    }
    const auto port = ntohs (address.sin_port);
    ::close (fd);
    std::ostringstream stream;
    stream << "tcp://127.0.0.1:" << port;
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

zlink::framework::runtime::messaging::message_parts_t
copy_message_parts (const std::vector<zlink::message_t> &parts)
{
    std::vector<zlink::message_t> copied;
    copied.reserve (parts.size ());
    for (const auto &part : parts) {
        copied.push_back (zlink::message_t::from (part.to_string ()));
    }
    return zlink::framework::runtime::messaging::message_parts_t (std::move (copied));
}

} // namespace

int main ()
{
    zlink::framework::zlink_builder_t zlink;
    zlink.add_node ("outbound-node");
    zlink.default_request_timeout (std::chrono::milliseconds (10000));
    zlink.channel ("profile")
      .default_request_timeout (std::chrono::milliseconds (1500))
      .enable_client ()
      .connect ("tcp://127.0.0.1:7101");
    zlink.channel ("events").enable_publisher ().bind ("tcp://127.0.0.1:7201");

    const auto channels = zlink::framework::detail::channel_runtime_t::from (zlink.message_bus ())
                            .channel_snapshots ();
    if (channels.size () != 2) {
        return 1;
    }

    zlink::framework::channel_builder_t standalone_channel;
    auto standalone_server = standalone_channel.enable_server ()
                               .bind ("tcp://127.0.0.1:7001")
                               .set_routing_id (zlink::routing_id_t::from ("standalone-server"))
                               .send_high_water_mark (zlink::byte_count_t::bytes (11))
                               .receive_high_water_mark (zlink::byte_count_t::bytes (12))
                               .max_message_size (zlink::byte_size_t::bytes (4096))
                               .peer_weight (zlink::peer_weight_t::value (3))
                               .snapshot ();
    standalone_channel.enable_client ().connect ("tcp://127.0.0.1:7002");
    standalone_channel.enable_publisher ().bind ("tcp://127.0.0.1:7003");
    standalone_channel.enable_subscriber ().connect ("tcp://127.0.0.1:7004");
    standalone_channel.default_request_timeout (std::chrono::milliseconds (250));
    const auto standalone_snapshot = standalone_channel.snapshot ();
    if (!standalone_server.enabled || standalone_server.bind_endpoints.size () != 1
        || !standalone_server.routing_id
        || standalone_server.routing_id->to_string () != "standalone-server"
        || !standalone_server.send_high_water_mark
        || standalone_server.send_high_water_mark->bytes () != 11
        || !standalone_server.receive_high_water_mark
        || standalone_server.receive_high_water_mark->bytes () != 12
        || !standalone_server.max_message_size
        || standalone_server.max_message_size->bytes () != 4096
        || !standalone_server.peer_weight || standalone_server.peer_weight->value () != 3
        || !standalone_snapshot.client.enabled || !standalone_snapshot.publisher.enabled
        || !standalone_snapshot.subscriber.enabled
        || standalone_snapshot.default_request_timeout != std::chrono::milliseconds (250)) {
        return 130;
    }
    bool invalid_channel_timeout_failed = false;
    try {
        standalone_channel.default_request_timeout (std::chrono::milliseconds (0));
    }
    catch (const zlink::framework::framework_exception_t &error) {
        invalid_channel_timeout_failed =
          error.kind () == zlink::framework::framework_error_kind_t::protocol_error;
    }
    if (!invalid_channel_timeout_failed) {
        return 131;
    }
    zlink::framework::capability_builder_t loose_capability;
    loose_capability.bind ("tcp://127.0.0.1:7051")
      .connect ("tcp://127.0.0.1:7052")
      .set_routing_id (zlink::routing_id_t::from ("loose-capability"))
      .send_high_water_mark (zlink::byte_count_t::bytes (4))
      .receive_high_water_mark (zlink::byte_count_t::bytes (5))
      .max_message_size (zlink::byte_size_t::bytes (2048))
      .peer_weight (zlink::peer_weight_t::value (6));
    zlink::framework::capability_builder_t moved_capability (std::move (loose_capability));
    zlink::framework::capability_builder_t assigned_capability;
    assigned_capability = std::move (moved_capability);
    const auto assigned_capability_snapshot = assigned_capability.snapshot ();
    if (!assigned_capability_snapshot.enabled
        || assigned_capability_snapshot.bind_endpoints.size () != 1
        || assigned_capability_snapshot.connect_endpoints.size () != 1
        || !assigned_capability_snapshot.routing_id
        || assigned_capability_snapshot.routing_id->to_string () != "loose-capability"
        || !assigned_capability_snapshot.peer_weight
        || assigned_capability_snapshot.peer_weight->value () != 6) {
        return 126;
    }
    zlink::framework::channel_builder_t movable_channel;
    movable_channel.enable_client ().connect ("tcp://127.0.0.1:7054");
    zlink::framework::channel_builder_t moved_channel (std::move (movable_channel));
    zlink::framework::channel_builder_t assigned_channel;
    assigned_channel = std::move (moved_channel);
    if (!assigned_channel.snapshot ().client.enabled) {
        return 124;
    }
    zlink::framework::zlink_builder_t movable_builder;
    movable_builder.add_node ("movable");
    zlink::framework::zlink_builder_t moved_builder (std::move (movable_builder));
    zlink::framework::zlink_builder_t assigned_builder;
    assigned_builder = std::move (moved_builder);
    assigned_builder.channel ("moved-channel").enable_client ().connect ("tcp://127.0.0.1:7053");
    const auto assigned_channels =
      zlink::framework::detail::channel_runtime_t::from (assigned_builder.message_bus ())
        .channel_snapshots ();
    if (assigned_channels.size () != 1 || assigned_channels[0].name != "moved-channel") {
        return 125;
    }
    zlink::framework::message_bus_t default_bus;
    zlink::framework::message_bus_t moved_bus (std::move (default_bus));
    zlink::framework::message_bus_t assigned_bus;
    assigned_bus = std::move (moved_bus);
    auto assigned_bus_runtime =
      zlink::framework::detail::channel_runtime_t::from (assigned_bus);
    if (assigned_bus_runtime.pending_count () != 0 || assigned_bus_runtime.pending_limit () == 0
        || assigned_bus.default_request_timeout ("missing-channel")
             <= std::chrono::milliseconds::zero ()
        || assigned_bus_runtime.server_peer_weight_override ("missing-channel")) {
        return 128;
    }
    zlink::framework::route_client_t default_route_client;
    /* Common spec 04-async-execution-policy.ko.md §1.3 (and its terminal table at the top
     * of §1) makes the one-way submit terminator complete exceptionally on failure rather
     * than throw out of submit(); the C++ terminal is task_t<void>, so the admission
     * failure is observed on the returned task. */
    const auto default_route_send =
      default_route_client
        .send_to_node ("missing.route", zlink::routing_id_t::from ("missing-node"), request_t{1})
        .metadata ("trace", "default")
        .submit ()
        .result ();
    if (default_route_send
        || default_route_send.error_kind ()
             != zlink::framework::framework_error_kind_t::protocol_error) {
        return 411;
    }
    auto default_route_request =
      default_route_client
        .request_to_node ("missing.route", zlink::routing_id_t::from ("missing-node"), request_t{2})
        .metadata ("trace", "default")
        .timeout (std::chrono::milliseconds (25))
        .submit<reply_t> ()
        .result ();
    auto default_spot_route_request =
      default_route_client
        .request_to_spot ("missing-spot", request_t{3})
        .submit<reply_t> ()
        .result ();
    if (default_route_request
        || default_route_request.error_kind ()
             != zlink::framework::framework_error_kind_t::protocol_error
        || default_spot_route_request
        || default_spot_route_request.error_kind ()
             != zlink::framework::framework_error_kind_t::not_configured) {
        return 127;
    }
    bool invalid_builder_timeout_failed = false;
    try {
        zlink.default_request_timeout (std::chrono::milliseconds (0));
    }
    catch (const zlink::framework::framework_exception_t &error) {
        invalid_builder_timeout_failed =
          error.kind () == zlink::framework::framework_error_kind_t::protocol_error;
    }
    bool empty_route_channel_failed = false;
    try {
        (void) zlink.route_channel ("");
    }
    catch (const zlink::framework::framework_exception_t &error) {
        empty_route_channel_failed =
          error.kind () == zlink::framework::framework_error_kind_t::protocol_error;
    }
    auto route_builder = zlink.route_channel ("runtime.route");
    route_builder.bind ("tcp://127.0.0.1:7401")
      .set_routing_id (zlink::routing_id_t::from ("runtime-route"))
      .connect ("tcp://127.0.0.1:7402")
      .default_request_timeout (std::chrono::milliseconds (275))
      .add_handler_group ("runtime-group");
    zlink::framework::route_channel_builder_t moved_route_builder (std::move (route_builder));
    zlink::framework::route_channel_builder_t assigned_route_builder;
    assigned_route_builder = std::move (moved_route_builder);
    assigned_route_builder.add_handler_group ("runtime-group-2");
    if (!invalid_builder_timeout_failed || !empty_route_channel_failed
        || zlink::framework::detail::channel_runtime_manager_t::configured_route_channel_ids (zlink)
             .empty ()) {
        return 129;
    }

    zlink.channel ("runtime-options").enable_server ().bind ("tcp://127.0.0.1:7301");
    zlink::framework::channel_runtime_options_t runtime_options (zlink.message_bus ());
    auto client_server_options = runtime_options.client_server_channel ("runtime-options");
    auto server_socket_options = client_server_options.configure_server_socket ();
    server_socket_options.peer_weight (zlink::peer_weight_t::value (7));
    zlink::framework::channel_server_socket_runtime_options_t moved_server_socket_options (
      std::move (server_socket_options));
    zlink::framework::channel_server_socket_runtime_options_t assigned_server_socket_options;
    assigned_server_socket_options = std::move (moved_server_socket_options);
    assigned_server_socket_options.peer_weight (zlink::peer_weight_t::value (8));
    auto runtime_options_weight =
      zlink::framework::detail::channel_runtime_t::from (zlink.message_bus ())
        .server_peer_weight_override ("runtime-options");
    if (!runtime_options_weight || *runtime_options_weight != 8) {
        return 132;
    }

    auto client = zlink.request_client ("profile");
    auto outbound_runtime =
      zlink::framework::detail::channel_runtime_t::from (zlink.message_bus ());
    auto request_call = client.request (request_t{1})
                          .metadata ("trace-id", "request-trace")
                          .timeout (std::chrono::milliseconds (3000));
    if (!outbound_runtime.outbound_calls ().empty ()) {
        return 30;
    }
    auto request_result = request_call.submit<reply_t> ().result ();
    if (request_result
        || (request_result.error () != nullptr
         && zlink::framework::detail::boundary_state (*request_result.error ()) != zlink::framework::detail::boundary_error_t::timed_out)) {
        return 2;
    }
    if (outbound_runtime.outbound_calls ().size () != 1
        || outbound_runtime.outbound_calls ()[0].kind != "request"
        || outbound_runtime.outbound_calls ()[0].packet_name != "profile.lookup"
        || outbound_runtime.outbound_calls ()[0].timeout != std::chrono::milliseconds (3000)
        || outbound_runtime.outbound_calls ()[0].metadata.at ("trace-id") != "request-trace") {
        return 31;
    }

    auto default_timeout_result =
      client.request (request_t{10}).submit<reply_t> ().result ();
    if (default_timeout_result
        || (default_timeout_result.error () != nullptr
         && zlink::framework::detail::boundary_state (*default_timeout_result.error ()) != zlink::framework::detail::boundary_error_t::timed_out)) {
        return 35;
    }
    if (outbound_runtime.outbound_calls ().size () != 2
        || outbound_runtime.outbound_calls ()[1].kind != "request"
        || outbound_runtime.outbound_calls ()[1].packet_name != "profile.lookup"
        || outbound_runtime.outbound_calls ()[1].timeout != std::chrono::milliseconds (1500)) {
        return 36;
    }

    auto bus = zlink.message_bus ();
    auto send_call = bus.send ("profile", request_t{2})
                       .metadata ("trace-id", "send-trace");
    if (outbound_runtime.outbound_calls ().size () != 2) {
        return 32;
    }
    send_call.submit ().result ().value ();
    if (outbound_runtime.outbound_calls ().size () != 3
        || outbound_runtime.outbound_calls ()[2].kind != "send"
        || outbound_runtime.outbound_calls ()[2].packet_name != "profile.lookup"
        || outbound_runtime.outbound_calls ()[2].metadata.at ("trace-id") != "send-trace") {
        return 33;
    }

    zlink.publisher ()
      .publish ("events", "profile.changed", event_t{3})
      .submit ().result ().value ();
    if (outbound_runtime.outbound_calls ().size () != 4
        || outbound_runtime.outbound_calls ()[3].kind != "publish"
        || outbound_runtime.outbound_calls ()[3].topic != "profile.changed"
        || outbound_runtime.outbound_calls ()[3].packet_name != "profile.changed.event") {
        return 34;
    }

    try {
        bus.send ("missing", request_t{4}).submit ().result ().value ();
    }
    catch (const zlink::framework::framework_exception_t &) {
        // A send to an unknown channel is rejected synchronously by the one-way
        // contract; this probe only checks that the runtime stays alive.
    }

    zlink::framework::zlink_builder_t full_queue;
    full_queue.max_pending (0);
    full_queue.channel ("profile").enable_client ();
    auto queue_full_result =
      full_queue.message_bus ().request ("profile", request_t{5}).submit<reply_t> ().result ();
    if (queue_full_result
        || queue_full_result.error_kind ()
             != zlink::framework::framework_error_kind_t::rejected) {
        return 6;
    }

    zlink::framework::zlink_builder_t outbound_only;
    auto outbound_only_channel = outbound_only.channel ("client-only");
    outbound_only_channel.enable_client ();
    outbound_only_channel.enable_publisher ();
    const auto outbound_channels =
      zlink::framework::detail::channel_runtime_t::from (outbound_only.message_bus ())
        .channel_snapshots ();
    if (outbound_channels.size () != 1 || outbound_channels[0].server.enabled
        || !outbound_channels[0].client.enabled || !outbound_channels[0].publisher.enabled) {
        return 7;
    }
    zlink::framework::zlink_builder_t fanout;
    auto fanout_channel = fanout.channel ("broadcast");
    fanout_channel.enable_publisher ().bind ("tcp://127.0.0.1:7351");
    fanout_channel.enable_subscriber ()
      .connect ("tcp://127.0.0.1:7351")
      .connect ("tcp://127.0.0.1:7352");
    auto fanout_manager = zlink::framework::detail::channel_runtime_manager_t::from (fanout);
    fanout_manager.initialize_publisher_channels ();
    fanout_manager.initialize_inbound_channels ();
    auto &fanout_publisher = fanout_manager.get_or_create_publisher_bundle ("broadcast");
    if (!fanout_publisher.contains_manual_connection ("tcp://127.0.0.1:7351")
        || fanout_manager.monitoring_source ("broadcast.publisher") != "broadcast.publisher"
        || fanout_manager.monitoring_source ("broadcast.subscriber") != "broadcast.subscriber") {
        return 74;
    }

    fanout.channel ("profile")
      .enable_server ()
      .set_routing_id (zlink::routing_id_t::from (std::string ("profile-api")))
      .bind ("tcp://127.0.0.1:7399");
    std::optional<zlink::framework::channel_snapshot_t> routed_server_snapshot;
    for (const auto &channel :
         zlink::framework::detail::channel_runtime_t::from (fanout.message_bus ())
           .channel_snapshots ()) {
        if (channel.name == "profile") {
            routed_server_snapshot = channel;
            break;
        }
    }
    if (!routed_server_snapshot || !routed_server_snapshot->server.routing_id
        || routed_server_snapshot->server.routing_id->to_string () != "profile-api") {
        return 75;
    }

    zlink::framework::zlink_builder_t local_server;
    local_server.channel ("local").enable_server ().bind ("tcp://127.0.0.1:7401");
    std::vector<zlink::framework::message_flow_event_t> dispatch_errors;
    std::mutex dispatch_errors_mutex;
    const auto dispatch_log_path =
      std::filesystem::temp_directory_path ()
      / ("zlink-cpp-derr-009-"
         + std::to_string (std::chrono::steady_clock::now ().time_since_epoch ().count ())
         + ".log");
    std::filesystem::remove (dispatch_log_path);
    zlink::framework::dispatch_options_t local_dispatch;
    local_dispatch.set_message_flow_observer (
      std::make_shared<recording_dispatch_observer_t> (dispatch_errors, dispatch_errors_mutex,
                                                       dispatch_log_path));
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
        != reported_before_no_observer + 1) {
        return 102;
    }

    zlink::framework::service_collection_t services;
    services.add_singleton<local_handler_t> ();
    services.add_singleton<throwing_handler_t> ();
    services.add_singleton<route_filter_t> ();
    auto provider = services.build_provider ();

    zlink::framework::serializer_registry_t serializers;
    add_int_serializer<request_t> (serializers);
    add_int_serializer<outer_route_request_t> (serializers);
    add_int_serializer<inner_route_request_t> (serializers);
    add_int_serializer<api_hop_request_t> (serializers);
    add_int_serializer<missing_probe_request_t> (serializers);
    add_int_serializer<reply_t> (serializers);
    add_int_serializer<event_t> (serializers);

    zlink::framework::detail::channel_runtime_t::from (outbound_only.message_bus ())
      .bind_serializers (serializers);
    zlink::framework::zlink_builder_t shutdown_outbound;
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
            return shutdown_outbound.message_bus ()
              .send ("shutdown-client", event_t{1})
              .submit ();
        })
        || !completes_shutdown ([&] {
            return shutdown_outbound.message_bus ()
              .publish ("shutdown-client", "events", event_t{2})
              .submit ();
        })) {
        return 407;
    }
    auto outbound_only_request =
      outbound_only.message_bus ().request ("client-only", request_t{6}).submit<reply_t> ().result ();
    if (outbound_only_request
        || (outbound_only_request.error () != nullptr
         && zlink::framework::detail::boundary_state (*outbound_only_request.error ()) != zlink::framework::detail::boundary_error_t::disconnected)) {
        return 404;
    }

    zlink::framework::route_send_call_t unbound_route_send ("event", {});
    /* One-way terminal rule again (04-async-execution-policy.ko.md §1.3): the unbound send
     * call reports its admission failure on the returned task_t<void>. */
    const auto unbound_route_send_result = unbound_route_send.submit ().result ();
    if (unbound_route_send_result
        || unbound_route_send_result.error_kind ()
             != zlink::framework::framework_error_kind_t::protocol_error) {
        return 408;
    }
    zlink::framework::channel_request_call_t unbound_route_request ("request", nullptr, {});
    const auto unbound_route_request_result = unbound_route_request.submit<reply_t> ().result ();
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
              .submit ();
        })) {
        return 409;
    }
    const auto unconfigured_route_request =
      unconfigured_route_client.request_to_node ("game.route", unconfigured_target, request_t{8})
        .submit<reply_t> ()
        .result ();
    if (unconfigured_route_request
        || unconfigured_route_request.error_kind ()
             != zlink::framework::framework_error_kind_t::protocol_error) {
        return 408;
    }
    const auto unconfigured_spot_send =
      unconfigured_route_client.send_to_spot (unconfigured_spot, event_t{9}).submit ().result ();
    if (unconfigured_spot_send
        || unconfigured_spot_send.error_kind ()
             != zlink::framework::framework_error_kind_t::not_configured) {
        return 410;
    }
    const auto unconfigured_spot_request =
      unconfigured_route_client
        .request_to_spot (unconfigured_spot, request_t{10})
        .submit<reply_t> ()
        .result ();
    if (unconfigured_spot_request
        || unconfigured_spot_request.error_kind ()
             != zlink::framework::framework_error_kind_t::not_configured) {
        return 410;
    }

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
    handlers.on_event<local_handler_t, event_t> ("local", "publish", &local_handler_t::handle_send,
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
             != 124) {
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
    const auto packet_error = packet_dispatcher.dispatch_server_message (
      "local", missing_parts, provider, serializers, handlers);
    if (!packet_error) {
        return 20;
    }
    const auto packet_error_header = envelope_codec.decode_header (packet_error.value ());
    if (!packet_error_header
        || packet_error_header.value ().kind
             != zlink::framework::runtime::messaging::message_kind_t::error
        || packet_error_header.value ().error_code.value_or ("") != "not_found") {
        return 21;
    }
    auto observed_dispatch_errors =
      wait_dispatch_errors (dispatch_errors, dispatch_errors_mutex, 1);
    if (observed_dispatch_errors.size () != 1
        || observed_dispatch_errors[0].surface
             != zlink::framework::dispatch_error_surface_t::channel
        || observed_dispatch_errors[0].message_kind
             != zlink::framework::dispatch_message_kind_t::request
        || observed_dispatch_errors[0].error_reason
             != zlink::framework::dispatch_error_reason_t::handler_missing
        || observed_dispatch_errors[0].error_action
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
    observed_dispatch_errors = wait_dispatch_errors (dispatch_errors, dispatch_errors_mutex, 1);
    if (observed_dispatch_errors.size () != 1
        || observed_dispatch_errors[0].surface
             != zlink::framework::dispatch_error_surface_t::channel
        || observed_dispatch_errors[0].message_kind
             != zlink::framework::dispatch_message_kind_t::send
        || observed_dispatch_errors[0].error_reason
             != zlink::framework::dispatch_error_reason_t::handler_missing
        || observed_dispatch_errors[0].error_action != zlink::framework::dispatch_error_action_t::drop
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
    auto publish_parts = envelope_codec.encode_raw_body_parts (
      publish_header, zlink::message_t::from (std::string ("34")));
    const auto packet_publish_result = packet_dispatcher.dispatch_server_message (
      "local", publish_parts, provider, serializers, handlers);
    if (!packet_publish_result || packet_publish_result.value ().size () != 0
        || provider.get_required<local_handler_t> ().last_event != 34) {
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
    observed_dispatch_errors = wait_dispatch_errors (dispatch_errors, dispatch_errors_mutex, 1);
    if (observed_dispatch_errors.size () != 1
        || observed_dispatch_errors[0].surface
             != zlink::framework::dispatch_error_surface_t::channel
        || observed_dispatch_errors[0].message_kind
             != zlink::framework::dispatch_message_kind_t::publish
        || observed_dispatch_errors[0].error_reason
             != zlink::framework::dispatch_error_reason_t::handler_missing
        || observed_dispatch_errors[0].error_action != zlink::framework::dispatch_error_action_t::drop
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
      provider.get_required<local_handler_t> ().last_request;
    auto malformed_parts = envelope_codec.encode_raw_body_parts (
      malformed_header, zlink::message_t::from (std::string ("not-an-int")));
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
        || provider.get_required<local_handler_t> ().last_request
             != last_request_before_malformed) {
        return 109;
    }
    observed_dispatch_errors = wait_dispatch_errors (dispatch_errors, dispatch_errors_mutex, 1);
    if (observed_dispatch_errors.size () != 1
        || observed_dispatch_errors[0].surface
             != zlink::framework::dispatch_error_surface_t::channel
        || observed_dispatch_errors[0].message_kind
             != zlink::framework::dispatch_message_kind_t::request
        || observed_dispatch_errors[0].error_reason
             != zlink::framework::dispatch_error_reason_t::payload_decode_failed
        || observed_dispatch_errors[0].error_action
             != zlink::framework::dispatch_error_action_t::reply_error
        || observed_dispatch_errors[0].packet_name.value_or ("") != "request"
        || observed_dispatch_errors[0].channel_name.value_or ("") != "local"
        || observed_dispatch_errors[0].topic.value_or ("") != "request"
        || observed_dispatch_errors[0].correlation_id.value_or ("") != "corr-payload-decode"
        || !observed_dispatch_errors[0].exception) {
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
             != "DERR-007 handler exception") {
        return 106;
    }
    observed_dispatch_errors = wait_dispatch_errors (dispatch_errors, dispatch_errors_mutex, 1);
    if (observed_dispatch_errors.size () != 1
        || observed_dispatch_errors[0].surface
             != zlink::framework::dispatch_error_surface_t::channel
        || observed_dispatch_errors[0].message_kind
             != zlink::framework::dispatch_message_kind_t::request
        || observed_dispatch_errors[0].error_reason
             != zlink::framework::dispatch_error_reason_t::handler_exception
        || observed_dispatch_errors[0].error_action
             != zlink::framework::dispatch_error_action_t::reply_error
        || observed_dispatch_errors[0].packet_name.value_or ("") != "throw"
        || observed_dispatch_errors[0].channel_name.value_or ("") != "local"
        || observed_dispatch_errors[0].topic.value_or ("") != "request"
        || observed_dispatch_errors[0].correlation_id.value_or ("") != "corr-handler-exception"
        || !observed_dispatch_errors[0].exception) {
        return 107;
    }
    clear_dispatch_errors (dispatch_errors, dispatch_errors_mutex);
    const auto dispatch_log_text = read_file (dispatch_log_path);
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
        if (native_server.recv (native_received) != 0 || !native_received.request_seq ()
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
                                  .async ();
    const auto native_client_reply = copy_message_parts (native_client_future.get ());
    const int native_server_result = native_server_done.get ();
    if (native_server_result != 0) {
        return native_server_result;
    }
    const auto decoded_native_reply = client_codec.decode_envelope_reply<reply_t> (
      native_client_reply, serializers, "native channel reply was empty",
      "native channel reply decode failed", "native channel request");
    if (!decoded_native_reply || decoded_native_reply.value ().value != 126) {
        return 79;
    }

    zlink::framework::zlink_builder_t native_bus_builder;
    const auto native_bus_endpoint = unique_tcp_endpoint ();
    native_bus_builder.channel ("native-bus").enable_client ().connect (native_bus_endpoint);
    zlink::framework::detail::channel_runtime_t::from (native_bus_builder.message_bus ())
      .bind_serializers (serializers);

    zlink::context_t native_bus_server_context;
    zlink::router_socket_t native_bus_server (native_bus_server_context);
    zlink::socket_monitor_t native_bus_server_monitor = native_bus_server.monitor_open ();
    native_bus_server.bind (native_bus_endpoint);
    auto native_bus_server_done = std::async (std::launch::async, [&] () -> int {
        for (int request_index = 0; request_index < 2; ++request_index) {
            zlink::received_t native_received;
            if (native_bus_server.recv (native_received) != 0 || !native_received.request_seq ()) {
                return 80;
            }
            const auto native_dispatch_reply = packet_dispatcher.dispatch_server_message (
              "local", copy_message_parts (native_received.parts ()), provider, serializers,
              handlers);
            if (!native_dispatch_reply || native_dispatch_reply.value ().size () != 2) {
                return 81;
            }
            zlink::message_t reply_header =
              zlink::message_t::from (native_dispatch_reply.value ()[0].to_string ());
            zlink::message_t reply_body =
              zlink::message_t::from (native_dispatch_reply.value ()[1].to_string ());
            native_received.reply ().message (reply_header).message (reply_body).submit ();
        }
        return 0;
    });
    auto native_bus_reply = native_bus_builder.request_client ("native-bus")
                              .request (request_t{27})
                              .timeout (std::chrono::milliseconds (2000))
                              .submit<reply_t> ()
                              .result ();
    if (!native_bus_reply || native_bus_reply.value ().value != 127) {
        return 82;
    }
    auto native_bus_missing_reply = native_bus_builder.request_client ("native-bus")
                                      .request (missing_probe_request_t{27})
                                      .timeout (std::chrono::milliseconds (2000))
                                      .submit<reply_t> ()
                                      .result ();
    if (native_bus_missing_reply
        || native_bus_missing_reply.error_kind ()
             != zlink::framework::framework_error_kind_t::not_found) {
        return 246;
    }
    const int native_bus_server_result = native_bus_server_done.get ();
    if (native_bus_server_result != 0) {
        return native_bus_server_result;
    }

    zlink::framework::runtime::messaging::envelope_header_t validation_header;
    validation_header.kind = zlink::framework::runtime::messaging::message_kind_t::response;
    validation_header.channel_name = "native-bus";
    validation_header.message_name = "request";
    validation_header.topic = "request";
    validation_header.correlation_id = "native-reply-validation";
    auto valid_native_reply = envelope_codec.encode_raw_body_parts (
      validation_header, zlink::message_t::from (std::string ("32")));
    if (!zlink::framework::detail::validate_channel_native_reply (valid_native_reply)) {
        return 242;
    }
    std::vector<zlink::message_t> one_frame_reply;
    one_frame_reply.push_back (zlink::message_t::from (std::string ("bad-header")));
    if (zlink::framework::detail::validate_channel_native_reply (
          zlink::framework::runtime::messaging::message_parts_t (std::move (one_frame_reply)))) {
        return 243;
    }
    std::vector<zlink::message_t> three_frame_reply;
    three_frame_reply.push_back (zlink::message_t::from (valid_native_reply[0].to_string ()));
    three_frame_reply.push_back (zlink::message_t::from (valid_native_reply[1].to_string ()));
    three_frame_reply.push_back (zlink::message_t::from (std::string ("extra-frame")));
    if (zlink::framework::detail::validate_channel_native_reply (
          zlink::framework::runtime::messaging::message_parts_t (std::move (three_frame_reply)))) {
        return 244;
    }
    validation_header.kind = zlink::framework::runtime::messaging::message_kind_t::request;
    auto request_kind_reply = envelope_codec.encode_raw_body_parts (
      validation_header, zlink::message_t::from (std::string ("32")));
    if (zlink::framework::detail::validate_channel_native_reply (request_kind_reply)) {
        return 245;
    }

    zlink::framework::zlink_builder_t hosted_builder;
    const auto hosted_endpoint = unique_tcp_endpoint ();
    const auto hosted_server_rid = zlink::routing_id_t::from (std::string ("hosted-server"));
    auto hosted_channel = hosted_builder.channel ("hosted");
    hosted_channel.enable_server ().set_routing_id (hosted_server_rid).bind (hosted_endpoint);
    hosted_channel.enable_client ().connect (hosted_endpoint);
    zlink::framework::detail::channel_runtime_t::from (hosted_builder.message_bus ())
      .bind_serializers (serializers);
    auto hosted_completion_admission =
      std::make_shared<
        zlink::framework::runtime::completion_admission_owner_t> (1);
    zlink::framework::runtime::channel_host_service_t hosted_service (
      hosted_builder.message_bus (),
      zlink::framework::detail::channel_runtime_t::from (hosted_builder.message_bus ())
        .channel_snapshots (),
      handlers, serializers, nullptr, hosted_completion_admission);
    hosted_service.start (provider);
    auto hosted_reply = hosted_builder.request_client ("hosted")
                          .request (request_t{28})
                          .timeout (std::chrono::milliseconds (2000))
                          .submit<reply_t> ()
                          .result ();
    if (!hosted_reply || hosted_reply.value ().value != 128) {
        hosted_service.stop ();
        return 83;
    }
    auto hosted_bus_reply = hosted_builder.message_bus ()
                              .request ("hosted", request_t{29})
                              .timeout (std::chrono::milliseconds (2000))
                              .submit<reply_t> ()
                              .result ();
    if (!hosted_bus_reply || hosted_bus_reply.value ().value != 129) {
        hosted_service.stop ();
        return 246;
    }
    hosted_builder.message_bus ().send ("hosted", event_t{30}).submit ();
    for (int attempt = 0;
         attempt < 50 && provider.get_required<local_handler_t> ().last_event != 30; ++attempt) {
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
    if (provider.get_required<local_handler_t> ().last_event != 30) {
        hosted_service.stop ();
        return 87;
    }
    auto &hosted_handler = provider.get_required<local_handler_t> ();
    hosted_builder.message_bus ().send ("hosted", event_t{300}).submit ();
    bool blocking_send_started = false;
    {
        std::unique_lock lock (hosted_handler.send_gate_mutex);
        blocking_send_started =
          hosted_handler.send_gate_changed.wait_for (
            lock, std::chrono::seconds (1),
            [&] { return hosted_handler.blocking_send_entered; });
    }
    if (!blocking_send_started) {
        hosted_service.stop ();
        return 247;
    }
    if (hosted_completion_admission->snapshot ().pending_completion_sends
        != 0) {
        {
            std::lock_guard lock (hosted_handler.send_gate_mutex);
            hosted_handler.release_blocking_send = true;
        }
        hosted_handler.send_gate_changed.notify_all ();
        hosted_service.stop ();
        return 248;
    }
    {
        std::lock_guard lock (hosted_handler.send_gate_mutex);
        hosted_handler.release_blocking_send = true;
    }
    hosted_handler.send_gate_changed.notify_all ();
    zlink::context_t peer_context;
    zlink::router_socket_t peer_router (peer_context);
    peer_router.connect (hosted_endpoint);
    zlink::framework::runtime::messaging::envelope_header_t hosted_header;
    hosted_header.kind = zlink::framework::runtime::messaging::message_kind_t::request;
    hosted_header.channel_name = "hosted";
    hosted_header.message_name = "request";
    hosted_header.topic = "request";
    hosted_header.correlation_id = "hosted-router-request";
    auto hosted_parts = envelope_codec.encode_raw_body_parts (
      hosted_header, zlink::message_t::from (std::string ("29")));
    std::vector<zlink::message_t> routed_hosted_reply;
    bool routed_request_completed = false;
    for (int attempt = 0; attempt < 50 && !routed_request_completed; ++attempt) {
        try {
            auto attempt_header = zlink::message_t::from (hosted_parts[0].to_string ());
            auto attempt_body = zlink::message_t::from (hosted_parts[1].to_string ());
            routed_hosted_reply = peer_router.request (hosted_server_rid)
                                    .message (attempt_header)
                                    .message (attempt_body)
                                    .timeout (std::chrono::milliseconds (200))
                                    .async ()
                                    .get ();
            routed_request_completed = true;
        }
        catch (const std::exception &) {
            std::this_thread::sleep_for (std::chrono::milliseconds (20));
        }
    }
    if (routed_hosted_reply.size () != 2) {
        hosted_service.stop ();
        return 85;
    }
    auto routed_reply_parts = copy_message_parts (routed_hosted_reply);
    const auto routed_reply_header = envelope_codec.decode_header (routed_reply_parts);
    const auto routed_reply_body = envelope_codec.decode_body (routed_reply_parts);
    if (!routed_reply_header
        || routed_reply_header.value ().kind
             != zlink::framework::runtime::messaging::message_kind_t::response
        || routed_reply_header.value ().correlation_id != "hosted-router-request"
        || !routed_reply_body
        || serializers.get<reply_t> ()
               .deserialize (
                 zlink::framework::detail::encoded_payload_from_raw (routed_reply_body.value ()))
               .value
             != 129) {
        hosted_service.stop ();
        return 86;
    }
    hosted_service.stop ();

    const auto manual_hosted_endpoint = unique_tcp_endpoint ();

    zlink::framework::zlink_builder_t manual_server_builder;
    manual_server_builder.channel ("hosted-manual")
      .enable_server ()
      .bind (manual_hosted_endpoint);
    zlink::framework::detail::channel_runtime_t::from (manual_server_builder.message_bus ())
      .bind_serializers (serializers);
    zlink::framework::runtime::channel_host_service_t manual_hosted_service (
      manual_server_builder.message_bus (),
      zlink::framework::detail::channel_runtime_t::from (manual_server_builder.message_bus ())
        .channel_snapshots (), handlers,
      serializers);
    manual_hosted_service.start (provider);

    zlink::framework::zlink_builder_t manual_client_builder;
    manual_client_builder.channel ("hosted-manual")
      .enable_client ()
      .connect (manual_hosted_endpoint);
    auto manual_client_runtime =
      zlink::framework::detail::channel_runtime_t::from (manual_client_builder.message_bus ());
    manual_client_runtime.bind_serializers (serializers);

    bool manual_reply_completed = false;
    reply_t manual_reply_value;
    const auto manual_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
    while (std::chrono::steady_clock::now () < manual_deadline && !manual_reply_completed) {
        auto manual_reply = manual_client_builder.request_client ("hosted-manual")
                              .request (request_t{33})
                              .timeout (std::chrono::milliseconds (500))
                              .submit<reply_t> ()
                              .result ();
        if (manual_reply) {
            manual_reply_value = manual_reply.value ();
            manual_reply_completed = true;
            break;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (25));
    }
    if (!manual_reply_completed || manual_reply_value.value != 133) {
        manual_hosted_service.stop ();
        return 88;
    }
    auto outage_reply = manual_client_builder.request_client ("hosted-manual")
                          .request (request_t{35})
                          .timeout (std::chrono::milliseconds (1000))
                          .submit<reply_t> ()
                          .result ();
    if (!outage_reply || outage_reply.value ().value != 135) {
        manual_hosted_service.stop ();
        return 90;
    }
    manual_hosted_service.stop ();
    const auto stale_start = std::chrono::steady_clock::now ();
    auto stale_reply = manual_client_builder.request_client ("hosted-manual")
                         .request (request_t{34})
                         .timeout (std::chrono::milliseconds (100))
                         .submit<reply_t> ()
                         .result ();
    const auto stale_elapsed = std::chrono::steady_clock::now () - stale_start;
    if (stale_reply
        || (stale_reply.error () != nullptr
         && zlink::framework::detail::boundary_state (*stale_reply.error ()) != zlink::framework::detail::boundary_error_t::timed_out)
        || stale_elapsed > std::chrono::seconds (2)) {
        return 89;
    }

    zlink::framework::zlink_builder_t nested_hosted_builder;
    const auto nested_hosted_endpoint = unique_tcp_endpoint ();
    auto nested_hosted_channel = nested_hosted_builder.channel ("hosted-nested");
    nested_hosted_channel.enable_server ().bind (nested_hosted_endpoint);
    nested_hosted_channel.enable_client ().connect (nested_hosted_endpoint);
    zlink::framework::detail::channel_runtime_t::from (nested_hosted_builder.message_bus ())
      .bind_serializers (serializers);
    zlink::framework::service_collection_t nested_services;
    nested_services.add_singleton<zlink::framework::channel_client_t> (
      std::make_unique<zlink::framework::channel_client_t> (nested_hosted_builder.message_bus ()));
    nested_services.add_transient<nested_request_handler_t, zlink::framework::channel_client_t> ();
    auto nested_provider = nested_services.build_provider ();
    zlink::framework::handler_registry_t nested_handlers;
    nested_handlers.on_request<nested_request_handler_t, request_t, reply_t> (
      "hosted-nested", "request", &nested_request_handler_t::handle_request,
      {.packet_name = request_t::packet_name});
    zlink::framework::runtime::channel_host_service_t nested_hosted_service (
      nested_hosted_builder.message_bus (),
      zlink::framework::detail::channel_runtime_t::from (nested_hosted_builder.message_bus ())
        .channel_snapshots (), nested_handlers,
      serializers);
    nested_hosted_service.start (nested_provider);
    auto nested_hosted_reply = nested_hosted_builder.request_client ("hosted-nested")
                                 .request (request_t{50})
                                 .timeout (std::chrono::milliseconds (2000))
                                 .submit<reply_t> ()
                                 .result ();
    nested_hosted_service.stop ();
    if (!nested_hosted_reply || nested_hosted_reply.value ().value != 152) {
        return 84;
    }

    zlink::framework::zlink_builder_t scoped_hosted_builder;
    const auto scoped_hosted_endpoint = unique_tcp_endpoint ();
    auto scoped_hosted_channel = scoped_hosted_builder.channel ("hosted-scoped");
    scoped_hosted_channel.enable_server ().bind (scoped_hosted_endpoint);
    scoped_hosted_channel.enable_client ().connect (scoped_hosted_endpoint);
    zlink::framework::detail::channel_runtime_t::from (scoped_hosted_builder.message_bus ())
      .bind_serializers (serializers);
    zlink::framework::service_collection_t scoped_services;
    scoped_services.add_scoped<scoped_channel_dependency_t> ();
    scoped_services
      .add_transient<scoped_channel_filter_t,
                     scoped_channel_dependency_t> ();
    scoped_services.add_transient<scoped_channel_handler_t, scoped_channel_dependency_t> ();
    auto scoped_provider = scoped_services.build_provider ();
    zlink::framework::handler_registry_t scoped_handlers;
    scoped_handlers.use_filter<scoped_channel_filter_t> ();
    scoped_handlers.on_request<scoped_channel_handler_t, request_t, reply_t> (
      "hosted-scoped", "request", &scoped_channel_handler_t::handle_request,
      {.packet_name = request_t::packet_name});
    zlink::framework::runtime::channel_host_service_t scoped_hosted_service (
      scoped_hosted_builder.message_bus (),
      zlink::framework::detail::channel_runtime_t::from (scoped_hosted_builder.message_bus ())
        .channel_snapshots (), scoped_handlers,
      serializers);
    scoped_hosted_service.start (scoped_provider);
    auto scoped_hosted_reply = scoped_hosted_builder.request_client ("hosted-scoped")
                                 .request (request_t{40})
                                 .timeout (std::chrono::milliseconds (2000))
                                 .submit<reply_t> ()
                                 .result ();
    auto second_scoped_reply =
      scoped_hosted_builder.request_client ("hosted-scoped")
        .request (request_t{41})
        .timeout (std::chrono::milliseconds (2000))
        .submit<reply_t> ()
        .result ();
    scoped_hosted_service.stop ();
    if (!scoped_hosted_reply || scoped_hosted_reply.value ().value != 440
        || !second_scoped_reply || second_scoped_reply.value ().value != 441
        || scoped_channel_dependency_t::created.load () != 2
        || scoped_channel_dependency_t::destroyed.load () != 2) {
        return 85;
    }

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
    const std::string reply_error_codes[] = {"unavailable",
                                             "not_found",
                                             "not_found",
                                             "rejected",
                                             "protocol_error",
                                             "deadline_exceeded",
                                             "shutting_down",
                                             "unavailable",
                                             "unavailable",
                                             "internal_failure"};
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
    if (!local_send || provider.get_required<local_handler_t> ().last_event != 31) {
        return 10;
    }

    auto not_server =
      local_runtime.dispatch_request ("profile", "request", "request", provider, serializers,
                                      handlers, zlink::message_t::from (std::string ("1")));
    if (not_server
        || not_server.error_kind ()
             != zlink::framework::framework_error_kind_t::unavailable) {
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
        || reentrant_result.error_kind ()
             != zlink::framework::framework_error_kind_t::rejected) {
        return 29;
    }

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
                    [] (const auto &target) {
                        return target.endpoint == "tcp://route-a:7500";
                    });
    if (upgraded_route_a == route_connection_targets.end () || !upgraded_route_a->peer_rid
        || *upgraded_route_a->peer_rid != route_peer) {
        return 312;
    }
    if (route_connections.connect (route_peer, "tcp://route-a:7500")) {
        return 313;
    }
    if (!route_connections.disconnect ("tcp://route-b:7500")
        || route_connections.contains ("tcp://route-b:7500")) {
        return 32;
    }

    zlink::framework::detail::route_channel_runtime_t route_runtime ("game.route");
    const auto target_node = zlink::routing_id_t::from (std::string ("remote-node"));
    auto disconnected_send =
      route_runtime.submit_send (target_node, "event", event_t{77}, serializers);
    if (disconnected_send
        || disconnected_send.error_kind ()
             != zlink::framework::framework_error_kind_t::unavailable) {
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
    auto spot_request =
      route_runtime.request_to_spot_parts (target_node, target_spot, request_parts);
    if (!spot_request || route_runtime.pending_request_count () != 2
        || route_runtime.outbound_packets ().back ().target_spot_id != target_spot) {
        return 38;
    }
    zlink::framework::detail::route_channel_runtime_t spot_backend_runtime ("spot.route");
    spot_backend_runtime.start ();
    spot_backend_runtime.connect ("tcp://spot-route-peer:7500");
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
    spot_backend_runtime.set_request_backend (
      [&] (const zlink::routing_id_t &target, const std::optional<std::string> &spot,
           const zlink::framework::runtime::messaging::message_parts_t &parts,
           std::chrono::milliseconds timeout) {
          if (target != target_node || spot != target_spot || parts.size () == 0
              || (timeout != std::chrono::seconds (30)
                  && timeout != std::chrono::milliseconds (25))) {
              return zlink::framework::
                result_t<zlink::framework::runtime::messaging::message_parts_t>::failure (
                  zlink::framework::framework_error_kind_t::internal_failure,
                  "unexpected spot request backend input");
          }
          ++spot_backend_requests;
          return zlink::framework::result_t<
            zlink::framework::runtime::messaging::message_parts_t>::success (parts);
      });
    if (!spot_backend_runtime.submit_spot_send_parts (target_node, target_spot, request_parts)
        || spot_backend_sends != 1) {
        return 380;
    }
    if (!spot_backend_runtime.request_to_spot_parts (target_node, target_spot, request_parts)
        || spot_backend_requests != 1 || spot_backend_runtime.pending_request_count () != 0) {
        return 381;
    }
    auto spot_backend_reply = spot_backend_runtime.request_reply_spot_parts (
      target_node, target_spot, request_parts, std::chrono::milliseconds (25));
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
              || timeout != std::chrono::milliseconds (25)) {
              return zlink::framework::
                result_t<zlink::framework::runtime::messaging::message_parts_t>::failure (
                  zlink::framework::framework_error_kind_t::internal_failure,
                  "unexpected auto route backend input");
          }
          ++auto_backend_requests;
          return zlink::framework::result_t<
            zlink::framework::runtime::messaging::message_parts_t>::success (parts);
      });
    auto auto_backend_reply = auto_backend_runtime.request_reply_parts (
      target_node, request_parts, std::chrono::milliseconds (25));
    if (!auto_backend_reply || auto_backend_reply.value ().size () != request_parts.size ()
        || auto_backend_requests != 1 || auto_backend_runtime.list_connections ().size () != 0) {
        return 383;
    }

    if (!route_runtime.complete_request (route_request.value ())
        || route_runtime.pending_request_count () != 1) {
        return 39;
    }
    route_runtime.stop ();
    if (route_runtime.running () || route_runtime.pending_request_count () != 0) {
        return 40;
    }

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
    observed_dispatch_errors = wait_dispatch_errors (dispatch_errors, dispatch_errors_mutex, 1);
    if (observed_dispatch_errors.size () != 1
        || observed_dispatch_errors[0].surface
             != zlink::framework::dispatch_error_surface_t::route_mesh_channel
        || observed_dispatch_errors[0].message_kind
             != zlink::framework::dispatch_message_kind_t::request
        || observed_dispatch_errors[0].error_reason
             != zlink::framework::dispatch_error_reason_t::handler_missing
        || observed_dispatch_errors[0].error_action
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
    test_route_receive_pump_t route_handler_pump{
      zlink::framework::detail::route_packet_dispatcher_t ("game.route", provider, serializers,
                                                           route_handlers, no_internal)};
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
        || provider.get_required<local_handler_t> ().last_route_request != 24
        || provider.get_required<local_handler_t> ().last_route_source != "source-node") {
        return 49;
    }
    route_handler_pump.enqueue (zlink::framework::detail::route_received_packet_t{
      zlink::routing_id_t::from (std::string ("source-node")), std::nullopt,
      route_runtime.outbound_packets ()[0].parts});
    const auto route_send_receive = route_handler_pump.drain ();
    if (!route_send_receive || !route_send_receive.value ().replies.empty ()
        || provider.get_required<local_handler_t> ().last_route_event != 78) {
        return 50;
    }

    zlink::framework::handler_registry_t route_filters;
    route_filters.use_filter<route_filter_t> ();
    auto &route_filter = provider.get_required<route_filter_t> ();
    route_filter.reject_request = true;
    provider.get_required<local_handler_t> ().last_route_request = 0;
    test_route_receive_pump_t filtered_route_pump{
      zlink::framework::detail::route_packet_dispatcher_t (
        "game.route", provider, serializers, route_handlers, no_internal,
        local_dispatch, &route_filters)};
    filtered_route_pump.enqueue (
      zlink::framework::detail::route_received_packet_t{
        zlink::routing_id_t::from (std::string ("source-node")), 79,
        route_request_parts});
    const auto filtered_request = filtered_route_pump.drain ();
    if (!filtered_request || filtered_request.value ().replies.size () != 1
        || provider.get_required<local_handler_t> ().last_route_request != 0
        || route_filter.seen_kinds.empty ()
        || route_filter.seen_kinds.back ()
             != zlink::framework::handler_dispatch_kind_t::
               node_direct_request
        || route_filter.last_mesh != "game.route"
        || route_filter.last_channel != "<none>") {
        return 409;
    }
    const auto filtered_error = envelope_codec.decode_header (
      filtered_request.value ().replies.front ().parts);
    if (!filtered_error
        || filtered_error.value ().kind
             != zlink::framework::runtime::messaging::message_kind_t::error
        || filtered_error.value ().error_code.value_or ("")
             != "rejected") {
        return 410;
    }

    route_filter.reject_request = false;
    route_filter.suppress_send = true;
    provider.get_required<local_handler_t> ().last_route_event = 0;
    filtered_route_pump.enqueue (
      zlink::framework::detail::route_received_packet_t{
        zlink::routing_id_t::from (std::string ("source-node")),
        std::nullopt, route_runtime.outbound_packets ()[0].parts});
    const auto filtered_send = filtered_route_pump.drain ();
    if (!filtered_send || !filtered_send.value ().replies.empty ()
        || provider.get_required<local_handler_t> ().last_route_event != 0
        || route_filter.seen_kinds.back ()
             != zlink::framework::handler_dispatch_kind_t::
               node_direct_send) {
        return 411;
    }

    route_filter.suppress_send = false;
    test_route_receive_pump_t filtered_channel_pump{
      zlink::framework::detail::route_packet_dispatcher_t (
        "game.route", provider, serializers, route_handlers, no_internal,
        local_dispatch, &route_filters,
        zlink::framework::handler_dispatch_kind_t::channel_send,
        zlink::framework::handler_dispatch_kind_t::channel_request)};
    filtered_channel_pump.enqueue (
      zlink::framework::detail::route_received_packet_t{
        zlink::routing_id_t::from (std::string ("source-node")), 80,
        route_request_parts});
    const auto filtered_channel_request = filtered_channel_pump.drain ();
    if (!filtered_channel_request
        || filtered_channel_request.value ().replies.size () != 1
        || route_filter.seen_kinds.back ()
             != zlink::framework::handler_dispatch_kind_t::
               channel_request) {
        return 412;
    }

    auto malformed_route_dispatch =
      zlink::framework::detail::route_packet_dispatcher_t ("game.route")
        .dispatch (zlink::framework::detail::route_received_packet_t{
          zlink::routing_id_t::from (std::string ("source-node")), std::nullopt,
          zlink::framework::runtime::messaging::message_parts_t (
            std::vector<zlink::message_t>{zlink::message_t::from (std::string ("not-json"))})});
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
          unsupported_route_parts});
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
            std::vector<zlink::message_t>{missing_body_header_only})});
    if (missing_body_dispatch
        || missing_body_dispatch.error_kind ()
             != zlink::framework::framework_error_kind_t::protocol_error) {
        return 403;
    }

    local_internal_dispatcher_t internal_a;
    zlink::framework::detail::composite_route_internal_packet_dispatcher_t composite_internal;
    composite_internal.add (internal_a);
    zlink::framework::runtime::messaging::envelope_header_t internal_header;
    internal_header.kind = zlink::framework::runtime::messaging::message_kind_t::request;
    internal_header.channel_name = "game.route";
    internal_header.message_name = "internal.request";
    internal_header.correlation_id = "internal-1";
    auto internal_parts = envelope_codec.encode_raw_body_parts (
      internal_header, zlink::message_t::from (std::string ("{}")));
    test_route_receive_pump_t internal_pump{zlink::framework::detail::route_packet_dispatcher_t (
      "game.route", provider, serializers, route_handlers, composite_internal)};
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
    if (provider.get_required<local_handler_t> ().internal_dispatch_provider_seen != 2) {
        return 64;
    }
    if (no_internal.can_handle_send ("internal.send")
        || no_internal.can_handle_request ("internal.request")) {
        return 65;
    }
    const auto no_internal_send = no_internal.dispatch_send (
      zlink::framework::detail::route_received_packet_t{
        zlink::routing_id_t::from (std::string ("source-node")), std::nullopt, internal_parts},
      provider);
    if (no_internal_send
        || no_internal_send.error_kind ()
             != zlink::framework::framework_error_kind_t::not_found) {
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
    if (!composite_internal.can_handle_send ("internal.send")
        || !composite_internal.dispatch_send (
          zlink::framework::detail::route_received_packet_t{
            zlink::routing_id_t::from (std::string ("source-node")), std::nullopt,
            internal_send_parts},
          provider)
        || internal_a.send_count != 1
        || provider.get_required<local_handler_t> ().internal_dispatch_provider_seen != 1) {
        return 68;
    }
    zlink::framework::runtime::messaging::envelope_header_t unsupported_internal_header;
    unsupported_internal_header.kind =
      zlink::framework::runtime::messaging::message_kind_t::command;
    unsupported_internal_header.channel_name = "game.route";
    unsupported_internal_header.message_name = "internal.unsupported";
    auto unsupported_internal_parts = envelope_codec.encode_raw_body_parts (
      unsupported_internal_header, zlink::message_t::from (std::string ("{}")));
    const auto unsupported_internal_send = composite_internal.dispatch_send (
      zlink::framework::detail::route_received_packet_t{
        zlink::routing_id_t::from (std::string ("source-node")), std::nullopt,
        unsupported_internal_parts},
      provider);
    if (unsupported_internal_send
        || unsupported_internal_send.error_kind ()
             != zlink::framework::framework_error_kind_t::not_found) {
        return 69;
    }
    unsupported_internal_header.kind =
      zlink::framework::runtime::messaging::message_kind_t::request;
    const auto unsupported_internal_request = composite_internal.dispatch_request (
      zlink::framework::detail::route_received_packet_t{
        zlink::routing_id_t::from (std::string ("source-node")), 82, unsupported_internal_parts},
      unsupported_internal_header, provider);
    if (unsupported_internal_request
        || unsupported_internal_request.error_kind ()
             != zlink::framework::framework_error_kind_t::not_found) {
        return 70;
    }
    const auto invalid_internal_send = composite_internal.dispatch_send (
      zlink::framework::detail::route_received_packet_t{
        zlink::routing_id_t::from (std::string ("source-node")), std::nullopt,
        zlink::framework::runtime::messaging::message_parts_t (
          std::vector<zlink::message_t>{zlink::message_t::from (std::string ("not-json"))})},
      provider);
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

    zlink::framework::zlink_builder_t public_route_builder;
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
    test_spot_address_resolver_t public_spot_resolver;
    public_spot_resolver.set (
      "target-spot", zlink::framework::runtime::spot_address_t{
                       "public.route", zlink::routing_id_t::from ("target-node"),
                       "target-spot"});
    zlink::framework::detail::channel_runtime_t::from (public_route_builder.message_bus ())
      .bind_spot_address_resolver (public_spot_resolver);
    auto public_route_client = public_route_builder.route_client (serializers);
    std::atomic_int send_backend_seen = 0;
    public_route.set_send_backend (
      [&send_backend_seen, &envelope_codec] (
        const zlink::routing_id_t &target, const std::optional<std::string> &spot,
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
      .send_to_node ("public.route", zlink::routing_id_t::from (std::string ("target-node")), event_t{31})
      .metadata ("trace-id", "trace-send")
      .submit ();
    const auto send_backend_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (1);
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
      [&envelope_codec, &serializers] (
        const zlink::routing_id_t &target, const std::optional<std::string> &spot,
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
        .submit<reply_t> ()
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
          const zlink::framework::runtime::messaging::message_parts_t &,
          std::chrono::milliseconds)
        -> zlink::framework::result_t<zlink::framework::runtime::messaging::message_parts_t> {
          return zlink::framework::result_t<
            zlink::framework::runtime::messaging::message_parts_t>::failure (
            zlink::framework::framework_error_kind_t::unavailable,
            "route peer is not connected");
      });
    auto missing_peer_reply =
      public_route_client
        .request_to_node ("public.route", zlink::routing_id_t::from (std::string ("target-node")),
                  request_t{50})
        .timeout (std::chrono::milliseconds (10))
        .submit<reply_t> ()
        .result ();
    if (missing_peer_reply
        || missing_peer_reply.error_kind ()
             != zlink::framework::framework_error_kind_t::unavailable
        || public_route.pending_request_count () != 0) {
        return 72;
    }
    public_route.set_request_backend (
      [&envelope_codec, &serializers] (
        const zlink::routing_id_t &target, const std::optional<std::string> &spot,
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
        .submit<reply_t> ()
        .result ();
    if (!public_typed_reply || public_typed_reply.value ().value != 351
        || public_route.outbound_packets ().size () != 4
        || public_route.pending_request_count () != 0) {
        return 62;
    }

    public_route.set_request_backend (
      [&envelope_codec] (
        const zlink::routing_id_t &, const std::optional<std::string> &,
        const zlink::framework::runtime::messaging::message_parts_t &parts,
        std::chrono::milliseconds)
        -> zlink::framework::result_t<zlink::framework::runtime::messaging::message_parts_t> {
          const auto request_header = envelope_codec.decode_header (parts);
          if (!request_header) {
              return zlink::framework::result_t<
                zlink::framework::runtime::messaging::message_parts_t>::failure (
                zlink::framework::framework_error_kind_t::internal_failure,
                "route error regression received an invalid request");
          }
          zlink::framework::runtime::messaging::envelope_header_t error_header;
          error_header.kind =
            zlink::framework::runtime::messaging::message_kind_t::error;
          error_header.channel_name = "public.route";
          error_header.message_name = request_header.value ().message_name;
          error_header.correlation_id = request_header.value ().correlation_id;
          error_header.error_code = "handler_not_found";
          error_header.error_message = "missing route handler";
          return zlink::framework::result_t<
            zlink::framework::runtime::messaging::message_parts_t>::success (
            envelope_codec.encode_raw_body_parts (error_header, zlink::message_t::from ("")));
      });
    const auto missing_route_handler =
      public_route_client
        .request_to_node ("public.route", zlink::routing_id_t::from (std::string ("target-node")),
                          request_t{52})
        .timeout (std::chrono::milliseconds (50))
        .submit<reply_t> ()
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
      [&spot_send_backend_seen, &envelope_codec] (
        const zlink::routing_id_t &target, const std::optional<std::string> &spot,
        const zlink::framework::runtime::messaging::message_parts_t &parts)
        -> zlink::framework::result_t<void> {
          auto header = envelope_codec.decode_header (parts);
          if (target.to_string () != "target-node" || !spot
              || *spot != "target-spot" || !header
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
    public_route_client
      .send_to_spot ("target-spot", event_t{32})
      .metadata ("trace-id", "trace-spot-send")
      .submit ();
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
      [&envelope_codec, &serializers] (
        const zlink::routing_id_t &target, const std::optional<std::string> &spot,
        const zlink::framework::runtime::messaging::message_parts_t &parts,
        std::chrono::milliseconds timeout)
        -> zlink::framework::result_t<zlink::framework::runtime::messaging::message_parts_t> {
          if (target.to_string () != "target-node" || !spot
              || *spot != "target-spot"
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
      public_route_client
        .request_to_spot ("target-spot", request_t{52})
        .metadata ("trace-id", "trace-spot-typed")
        .timeout (std::chrono::milliseconds (50))
        .submit<reply_t> ()
        .result ();
    if (!public_spot_typed_reply || public_spot_typed_reply.value ().value != 352) {
        return 77;
    }

    zlink::framework::zlink_builder_t spot_only_builder;
    auto spot_only_runtime = zlink::framework::detail::channel_runtime_t::from (
      spot_only_builder.message_bus ());
    test_spot_address_resolver_t spot_only_resolver;
    int spot_only_send_count = 0;
    int spot_only_node_send_count = 0;
    bool spot_only_request_called = false;
    /* The handle carries a concrete ObjectGeneration so the transport can assert the
     * exact generation the SPOT address snapshot published. */
    constexpr std::uint64_t spot_only_generation = 7;
    spot_only_resolver.set (
      "spot-rid", zlink::framework::runtime::spot_address_t{
                    "spot-only", zlink::routing_id_t::from ("spot-node"), "spot-rid",
                    spot_only_generation});
    spot_only_runtime.bind_spot_address_resolver (spot_only_resolver);
    spot_only_runtime.bind_spot_mesh_transport (
      "spot-only",
      [&spot_only_send_count] (
        const zlink::routing_id_t &target_node_rid,
        const std::string &target_spot_id,
        std::uint64_t target_spot_generation,
        zlink::framework::runtime::messaging::message_parts_t) {
          if (target_node_rid.to_string () == "spot-node" && target_spot_id == "spot-rid"
              && target_spot_generation == spot_only_generation) {
              ++spot_only_send_count;
          }
          return zlink::framework::result_t<void>::success ();
      },
      [&spot_only_request_called, &envelope_codec, &serializers] (
        const zlink::routing_id_t &target_node_rid,
        const std::string &target_spot_id,
        std::uint64_t target_spot_generation,
        zlink::framework::runtime::messaging::message_parts_t parts,
        std::chrono::milliseconds timeout) {
          const auto header = envelope_codec.decode_header (parts);
          const auto body = envelope_codec.decode_body (parts);
          if (!header || !body || target_node_rid.to_string () != "spot-node"
              || target_spot_id != "spot-rid"
              || target_spot_generation != spot_only_generation
              || timeout != std::chrono::milliseconds (75)
              || serializers.get<request_t> ()
                     .deserialize (zlink::framework::detail::encoded_payload_from_raw (
                       body.value ()))
                     .value
                   != 53) {
              return zlink::framework::result_t<
                zlink::framework::runtime::messaging::message_parts_t>::failure (
                zlink::framework::framework_error_kind_t::internal_failure,
                "spot-only transport received unexpected request");
          }
          spot_only_request_called = true;
          auto reply_header = header.value ();
          reply_header.kind = zlink::framework::runtime::messaging::message_kind_t::response;
          reply_t reply{353};
          return zlink::framework::result_t<
            zlink::framework::runtime::messaging::message_parts_t>::success (
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
      [&spot_only_node_send_count] (
        const zlink::routing_id_t &target_node_rid,
        zlink::framework::runtime::messaging::message_parts_t) {
          if (target_node_rid.to_string () == "spot-node") {
              ++spot_only_node_send_count;
          }
          return zlink::framework::result_t<void>::success ();
      },
      [] (const zlink::routing_id_t &, zlink::framework::runtime::messaging::message_parts_t,
          std::chrono::milliseconds) {
          return zlink::framework::result_t<
            zlink::framework::runtime::messaging::message_parts_t>::failure (
            zlink::framework::framework_error_kind_t::internal_failure,
            "spot-only node transport received an unexpected request");
      });
    auto spot_only_client = spot_only_builder.route_client (serializers);
    spot_only_client.send_to_spot ("spot-rid", event_t{33})
      .submit ();
    spot_only_client
      .send_to_node ("spot-only", zlink::routing_id_t::from ("spot-node"), event_t{34})
      .submit ();
    const auto spot_only_reply = spot_only_client
                                   .request_to_spot ("spot-rid", request_t{53})
                                   .timeout (std::chrono::milliseconds (75))
                                   .submit<reply_t> ()
                                   .result ();
    if (spot_only_send_count != 1 || spot_only_node_send_count != 1 || !spot_only_reply
        || !spot_only_request_called
        || spot_only_reply.value ().value != 353) {
        return 142;
    }

    /* A Missing direct Spot is activated only when the fluent call carries
     * Instance intent. The resulting Ready address is used by the same
     * operation; a later call resolves that address and does not activate it
     * again. */
    zlink::framework::zlink_builder_t activation_builder;
    auto activation_runtime = zlink::framework::detail::channel_runtime_t::from (
      activation_builder.message_bus ());
    test_spot_address_resolver_t activation_resolver;
    activation_runtime.bind_spot_address_resolver (activation_resolver);
    std::atomic_int activation_count{0};
    activation_runtime.bind_instance_spot_activator (
      [&] (const zlink::framework::spot_id_t &spot_id,
           const zlink::framework::detail::spot_activation_intent_t &intent,
           const std::string &, std::type_index, auto,
           const std::map<std::string, std::string> &) {
          if (std::string (spot_id) != "cart-17" || intent.mesh_name != "commerce"
              || intent.stable_type != "shopping-cart") {
              return zlink::framework::result_t<void>::failure (
                zlink::framework::framework_error_kind_t::not_configured,
                "unexpected Instance Spot activation intent");
          }
          ++activation_count;
          auto address = zlink::framework::runtime::spot_address_t{
            "commerce", zlink::routing_id_t::from ("cart-node"), "cart-17", 1};
          activation_resolver.set ("cart-17", address);
          return zlink::framework::result_t<void>::success ();
      },
      [] (const auto &, const auto &, auto, auto, auto, auto, auto) {
          return zlink::framework::task_t<zlink::message_t> (
            zlink::framework::result_t<zlink::message_t>::failure (
              zlink::framework::framework_error_kind_t::internal_failure,
              "Ready resolve should bypass cold activation"));
      });
    std::atomic_int activation_send_count{0};
    std::atomic_int activation_request_count{0};
    activation_runtime.bind_spot_mesh_transport (
      "commerce",
      [&] (const zlink::routing_id_t &target_node, const std::string &target_spot,
           std::uint64_t generation,
           zlink::framework::runtime::messaging::message_parts_t) {
          if (target_node.to_string () == "cart-node" && target_spot == "cart-17"
              && generation == 1) {
              ++activation_send_count;
          }
          return zlink::framework::result_t<void>::success ();
      },
      [&] (const zlink::routing_id_t &, const std::string &, std::uint64_t,
           zlink::framework::runtime::messaging::message_parts_t parts,
           std::chrono::milliseconds) {
          ++activation_request_count;
          const auto header = envelope_codec.decode_header (parts);
          auto reply_header = header.value ();
          reply_header.kind = zlink::framework::runtime::messaging::message_kind_t::response;
          reply_t reply{617};
          return zlink::framework::result_t<
            zlink::framework::runtime::messaging::message_parts_t>::success (
            envelope_codec.encode_parts (reply_header, std::type_index (typeid (reply_t)),
                                         &reply, serializers));
      });
    auto activation_client = activation_builder.route_client (serializers);
    const auto activation_send = activation_client
                                   .send_to_spot ("cart-17", event_t{61})
                                   .instance_spot ("shopping-cart")
                                   .in_mesh ("commerce")
                                   .submit ().result ();
    const auto activation_reply = activation_client
                                    .request_to_spot ("cart-17", request_t{62})
                                    .instance_spot ("ignored-for-ready-owner")
                                    .in_mesh ("other-mesh")
                                    .submit<reply_t> ().result ();
    if (!activation_send || !activation_reply
        || activation_reply.value ().value != 617
        || activation_count.load () != 1 || activation_send_count.load () != 0
        || activation_request_count.load () != 1) {
        return 150;
    }

    /* A stale resolved address fails the current operation without retry. The
     * next operation resolves the global SpotId again and may use a newer owner. */
    zlink::framework::zlink_builder_t retry_builder;
    auto retry_runtime =
      zlink::framework::detail::channel_runtime_t::from (retry_builder.message_bus ());
    test_spot_address_resolver_t retry_resolver;
    retry_resolver.set (
      "moving-spot", zlink::framework::runtime::spot_address_t{
                       "retry-mesh", zlink::routing_id_t::from ("retry-node"),
                       "stale-spot"});
    retry_runtime.bind_spot_address_resolver (retry_resolver);
    std::atomic_int retry_stale_attempts{0};
    std::atomic_int retry_fresh_attempts{0};
    retry_runtime.bind_spot_mesh_transport (
      "retry-mesh",
      [] (const zlink::routing_id_t &, const std::string &, std::uint64_t,
          zlink::framework::runtime::messaging::message_parts_t) {
          return zlink::framework::result_t<void>::success ();
      },
      [&retry_stale_attempts, &retry_fresh_attempts, &envelope_codec, &serializers] (
        const zlink::routing_id_t &, const std::string &target_spot_id, std::uint64_t,
        zlink::framework::runtime::messaging::message_parts_t parts, std::chrono::milliseconds) {
          if (target_spot_id == "stale-spot") {
              ++retry_stale_attempts;
              return zlink::framework::result_t<
                zlink::framework::runtime::messaging::message_parts_t>::failure (
                zlink::framework::framework_error_kind_t::not_found,
                "spot moved away from the stale address");
          }
          if (target_spot_id != "fresh-spot") {
              return zlink::framework::result_t<
                zlink::framework::runtime::messaging::message_parts_t>::failure (
                zlink::framework::framework_error_kind_t::internal_failure,
                "unexpected retry target");
          }
          ++retry_fresh_attempts;
          const auto header = envelope_codec.decode_header (parts);
          auto reply_header = header.value ();
          reply_header.kind = zlink::framework::runtime::messaging::message_kind_t::response;
          reply_t reply{454};
          return zlink::framework::result_t<
            zlink::framework::runtime::messaging::message_parts_t>::success (
            envelope_codec.encode_parts (reply_header, std::type_index (typeid (reply_t)), &reply,
                                         serializers));
      });
    auto retry_client = retry_builder.route_client (serializers);
    const auto stale_spot_reply = retry_client.request_to_spot ("moving-spot", request_t{54})
                                    .timeout (std::chrono::milliseconds (75))
                                    .submit<reply_t> ()
                                    .result ();
    if (stale_spot_reply
        || stale_spot_reply.error_kind ()
             != zlink::framework::framework_error_kind_t::not_found
        || retry_resolver.resolve_count.load () != 1 || retry_resolver.invalidate_count.load () != 1
        || retry_stale_attempts.load () != 1
        || retry_fresh_attempts.load () != 0) {
        return 143;
    }
    retry_resolver.set (
      "moving-spot", zlink::framework::runtime::spot_address_t{
                       "retry-mesh", zlink::routing_id_t::from ("retry-node"),
                       "fresh-spot"});
    const auto fresh_reply = retry_client.request_to_spot ("moving-spot", request_t{54})
                               .timeout (std::chrono::milliseconds (75))
                               .submit<reply_t> ()
                               .result ();
    if (!fresh_reply || fresh_reply.value ().value != 454
        || retry_resolver.resolve_count.load () != 2 || retry_stale_attempts.load () != 1
        || retry_fresh_attempts.load () != 1) {
        return 144;
    }

    /* endpoint_connections live attach: post-apply connect/disconnect on the
     * public handle mutates the same bundle set requests iterate. */
    {
        zlink::framework::zlink_builder_t live_builder;
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
    std::promise<void> release_delayed_backend;
    auto delayed_backend_entered_future = delayed_backend_entered.get_future ();
    auto release_delayed_backend_future = release_delayed_backend.get_future ().share ();
    std::atomic_bool delayed_completed = false;
    public_route.set_request_backend (
      [&delayed_backend_entered, release_delayed_backend_future, &envelope_codec, &serializers] (
        const zlink::routing_id_t &, const std::optional<std::string> &,
        const zlink::framework::runtime::messaging::message_parts_t &, std::chrono::milliseconds)
        -> zlink::framework::result_t<zlink::framework::runtime::messaging::message_parts_t> {
          delayed_backend_entered.set_value ();
          release_delayed_backend_future.wait ();
          zlink::framework::runtime::messaging::envelope_header_t reply_header;
          reply_header.kind = zlink::framework::runtime::messaging::message_kind_t::response;
          reply_header.channel_name = "public.route";
          reply_header.message_name = request_t::packet_name;
          reply_t reply{451};
          return zlink::framework::result_t<zlink::framework::runtime::messaging::message_parts_t>::
            success (envelope_codec.encode_parts (reply_header, std::type_index (typeid (reply_t)),
                                                  &reply, serializers));
      });
    auto delayed_task =
      public_route_client
        .request_to_node ("public.route", zlink::routing_id_t::from (std::string ("target-node")),
                  request_t{52})
        .timeout (std::chrono::milliseconds (50))
        .submit<reply_t> ();
    zlink::framework::detail::observe_task_completion (
      delayed_task, [&delayed_completed] (const zlink::framework::result_t<reply_t> &) {
          delayed_completed = true;
      });
    if (delayed_backend_entered_future.wait_for (std::chrono::seconds (1))
        != std::future_status::ready) {
        return 63;
    }
    if (delayed_completed.load ()) {
        return 64;
    }
    release_delayed_backend.set_value ();
    auto delayed_reply = delayed_task.result ();
    if (!delayed_reply || delayed_reply.value ().value != 451 || !delayed_completed.load ()) {
        return 65;
    }

    zlink::framework::detail::register_spot_route_packet_serializers (serializers);
    zlink::framework::zlink_builder_t stream_builder;
    stream_builder.stream ("routed-bound-session").bind ("tcp://0.0.0.0:9300");
    auto stream_runtime = zlink::framework::detail::stream_runtime_t::from (stream_builder);
    auto stream = stream_runtime.open_session ("routed-bound-session");
    zlink::framework::detail::actor_gateway_runtime_t actor_gateway;
    auto actor_ref = zlink::framework::detail::actor_ref_access_t::make (
      zlink::framework::node_rid_t::from_string ("play-node"), "PlayerActor", "observer", 3);
    auto bound_actor = actor_gateway.manager ().bind (actor_ref).submit ().result ();
    if (!bound_actor) {
        return 66;
    }
    actor_gateway.bind_session_stream ("observer", stream, zlink::framework::stream_codec_t::json);
    zlink::framework::detail::actor_route_internal_dispatcher_t actor_dispatcher (actor_gateway,
                                                                                  serializers);
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
    auto bound_parts = envelope_codec.encode_parts (
      bound_header,
      std::type_index (typeid (zlink::framework::detail::actor_bound_session_route_request_t)),
      &bound_request, serializers);
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
    auto bound_send_parts = envelope_codec.encode_parts (
      bound_header,
      std::type_index (typeid (zlink::framework::detail::actor_bound_session_route_request_t)),
      &bound_request, serializers);
    auto bound_send = actor_dispatcher.dispatch_send (
      zlink::framework::detail::route_received_packet_t{
        zlink::routing_id_t::from (std::string ("play-node")), 100, std::move (bound_send_parts)},
      provider);
    const auto routed_send_headers = stream_runtime.written_headers (stream);
    if (!bound_send || routed_send_headers.size () != 2
        || routed_send_headers[1].codec () != zlink::framework::stream_codec_t::json
        || routed_send_headers[1].packet_name () != "BingoRewardAnnouncedNotify") {
        return 73;
    }

    std::atomic_int route_submit_attempts{0};
    zlink::framework::route_send_call_t one_shot_route (
      "one-shot",
      [&] (const std::string &,
           const zlink::framework::route_send_call_t::metadata_map_t &) {
          ++route_submit_attempts;
          return zlink::framework::result_t<void>::success ();
      });
    auto copied_route = one_shot_route;
    one_shot_route.submit ().result ().value ();
    bool copied_route_rejected = false;
    try {
        (void) copied_route.submit ().result ().value ();
    }
    catch (const zlink::framework::framework_exception_t &error) {
        copied_route_rejected =
          error.kind ()
          == zlink::framework::framework_error_kind_t::protocol_error;
    }
    if (!copied_route_rejected || route_submit_attempts.load () != 1) {
        return 149;
    }

    return 0;
}
