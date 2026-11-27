/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once
#include "loopback_tcp_endpoint.hpp"
#include "runtime_failure_fixture.hpp"
#include "../Zlink.Framework.UnitTests/metric_test_reader.hpp"

#include <zlink/framework.hpp>

#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/listener_status_registry.hpp"
#include "runtime/dispatch/host_capacity_runtime.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/streams/stream_host_service.hpp"
#include "runtime/streams/stream_runtime.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/websocket.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace stream_liveness_test
{
namespace fw = zlink::framework;
namespace rt = fw::runtime;
namespace detail = fw::detail;
namespace asio = boost::asio;
namespace websocket = boost::beast::websocket;
using asio::ip::tcp;
using namespace std::chrono_literals;
using steady_clock = std::chrono::steady_clock;

constexpr auto receive_timeout = 1000ms;
constexpr auto server_ping_period = 200ms;
constexpr auto probe_period = 40ms;
constexpr auto observation_after_release = 500ms;

inline void require (bool condition, std::string_view message)
{
    if (!condition)
        throw std::runtime_error (std::string (message));
}

class session_callbacks_t final : public fw::packet_stream_session_t
{
  public:
    fw::task_t<void> on_connected (fw::stream_t &) override
    {
        ++connections;
        co_return;
    }

    fw::task_t<void> on_disconnected (fw::stream_t &) override
    {
        ++disconnections;
        co_return;
    }

    fw::task_t<void> on_error (fw::stream_t &, const fw::stream_error_t &) override
    {
        ++errors;
        co_return;
    }

    fw::task_t<void> on_packet (fw::stream_t &,
                                const fw::session_message_context_t &,
                                const fw::message_t &) override
    {
        ++packets;
        co_return;
    }

    std::atomic<int> connections{0};
    std::atomic<int> disconnections{0};
    std::atomic<int> errors{0};
    std::atomic<int> packets{0};
};

struct peer_observation_t
{
    int writes = 0;
    int pongs = 0;
    int server_pings = 0;
    std::optional<steady_clock::time_point> last_write;
    std::optional<steady_clock::time_point> closing;
    std::optional<steady_clock::time_point> closed;
    std::optional<fw::stream_close_reason_t> reason;
    boost::system::error_code read_error;
    boost::system::error_code write_error;
};

class ping_only_client_t
{
  public:
    ping_only_client_t (const std::string &endpoint, const detail::stream_runtime_t &runtime) :
        _runtime (runtime), _is_websocket (endpoint.starts_with ("ws:"))
    {
        const auto port = fw::tests::endpoint_port (endpoint);
        _socket.next_layer ().connect ({asio::ip::address_v4::loopback (), port});
        if (_is_websocket) {
            _socket.handshake ("127.0.0.1:" + std::to_string (port), "/");
            _socket.binary (true);
        }

        const detail::stream_header_t ping{
          detail::stream_message_kind_t::control, fw::stream_codec_t::raw,
          detail::stream_header_flags_t::none, std::nullopt, detail::heartbeat_ping_name};
        _ping = _runtime.encode_frame (ping, zlink::message_t{}).value ();
        asio::co_spawn (_io, read_controls (),
                        [this] (std::exception_ptr error) { _protocol_error = std::move (error); });
    }

    ~ping_only_client_t ()
    {
        boost::system::error_code ignored;
        _socket.next_layer ().close (ignored);
        _io.restart ();
        _io.run ();
    }

    const peer_observation_t &observation () const { return _observed; }
    bool write_pending () const { return _write_pending; }

    void advance ()
    {
        _io.restart ();
        _io.run_for (2ms);
        if (_protocol_error)
            std::rethrow_exception (_protocol_error);
    }

    void send_ping (bool partial = false)
    {
        if (_write_pending || _observed.closing || _observed.closed || _observed.write_error)
            return;
        _write_pending = true;
        const auto sent = [this] (boost::system::error_code error, std::size_t) {
            _write_pending = false;
            if (error) {
                _observed.write_error = error;
                return;
            }
            ++_observed.writes;
            _observed.last_write = steady_clock::now ();
        };
        const auto bytes = asio::buffer (_ping.data (), partial ? 1 : _ping.size ());
        if (_is_websocket)
            _socket.async_write_some (!partial, bytes, sent);
        else
            asio::async_write (_socket.next_layer (), bytes, sent);
    }

  private:
    static std::size_t big_endian_size (std::span<const std::uint8_t> bytes)
    {
        std::size_t size = 0;
        for (const auto byte : bytes)
            size = (size << 8) | byte;
        return size;
    }

    static constexpr std::size_t prefix_size = 6;

    asio::awaitable<std::vector<std::uint8_t>> read_packet ()
    {
        if (_is_websocket) {
            boost::beast::flat_buffer buffer;
            co_await _socket.async_read (buffer, asio::use_awaitable);
            require (_socket.got_binary (), "Server sent a non-binary WebSocket message");
            std::vector<std::uint8_t> packet (buffer.size ());
            asio::buffer_copy (asio::buffer (packet), buffer.data ());
            co_return packet;
        }

        std::vector<std::uint8_t> packet (prefix_size);
        co_await asio::async_read (_socket.next_layer (), asio::buffer (packet),
                                   asio::use_awaitable);
        const auto bytes = std::span (packet);
        const auto size = big_endian_size (bytes.first (2)) + big_endian_size (bytes.subspan (2));
        require (size <= 65536, "Server control packet is too large");
        packet.resize (prefix_size + size);
        co_await asio::async_read (_socket.next_layer (),
                                   asio::buffer (packet.data () + prefix_size, size),
                                   asio::use_awaitable);
        co_return packet;
    }

    void record_control (std::span<const std::uint8_t> packet)
    {
        require (packet.size () >= prefix_size, "Server control prefix is incomplete");
        const auto header_size = big_endian_size (packet.first (2));
        const auto body_size = big_endian_size (packet.subspan (2, 4));
        require (prefix_size + header_size + body_size == packet.size (),
                 "Server control packet size does not match its prefix");
        const auto header_bytes = packet.subspan (prefix_size, header_size);
        const auto header =
          _runtime.decode_header ({header_bytes.begin (), header_bytes.end ()}).value ();
        require (header.kind () == detail::stream_message_kind_t::control,
                 "Ping-only client received an application packet");
        const auto name = header.packet_name ();
        if (name == detail::heartbeat_pong_name) {
            ++_observed.pongs;
        } else if (name == detail::heartbeat_ping_name) {
            ++_observed.server_pings;
        } else {
            constexpr auto close_payload_prefix_size = 4;
            const auto body = packet.last (body_size);
            require (name == "session-closing" && body.size () >= close_payload_prefix_size
                       && body[0] == 1
                       && big_endian_size (body.subspan (2, 2))
                            == body.size () - close_payload_prefix_size,
                     "Invalid session-closing control");
            _observed.reason = static_cast<fw::stream_close_reason_t> (body[1]);
            _observed.closing = steady_clock::now ();
        }
    }

    asio::awaitable<void> read_controls ()
    {
        try {
            for (;;)
                record_control (co_await read_packet ());
        }
        catch (const boost::system::system_error &error) {
            _observed.read_error = error.code ();
            _observed.closed = steady_clock::now ();
        }
    }

    const detail::stream_runtime_t &_runtime;
    const bool _is_websocket;
    asio::io_context _io;
    websocket::stream<tcp::socket> _socket{_io};
    std::vector<std::uint8_t> _ping;
    peer_observation_t _observed;
    std::exception_ptr _protocol_error;
    bool _write_pending = false;
};

template <typename Test>
void with_stream_host (std::string_view transport,
                       std::chrono::milliseconds idle_timeout,
                       Test test)
{
    const bool core = transport == "core";
    metric_test::provider_t metrics;
    fw::service_collection_t services;
    services.add_singleton<detail::actor_gateway_runtime_t> ();
    services.add_factory<fw::session_actor_manager_t> (
      [] (fw::service_provider_t &provider) {
          return std::make_unique<fw::session_actor_manager_t> (
            provider.get_required<detail::actor_gateway_runtime_t> ().manager ());
      },
      fw::service_lifetime_t::scoped);
    fw::handler_registry_t handlers;
    fw::serializer_registry_t serializers;
    auto builder = fw::test::runtime_failure_builder ();
    fw::zlink_framework_options_t options (services, handlers, serializers, builder);
    auto context = std::make_shared<zlink::context_t> ();
    if (core) {
        detail::zlink_builder_access_t::bind_shared_core_context (builder, context);
        options.add_route_mesh ("liveness-mesh")
          .set_routing_id (zlink::routing_id_t::from ("liveness-node"))
          .listen (fw::tests::loopback_tcp_any_port);
    }
    options.add_stream_node ("liveness")
      .bind (transport == "websocket" ? "ws://127.0.0.1:*" : "tcp://127.0.0.1:*")
      .set_heartbeat (server_ping_period, receive_timeout)
      .set_idle_timeout (idle_timeout)
      .set_authentication_packet_name ("Login")
      .set_max_concurrent_authentications (1)
      .register_session ("probe");
    options.apply ();
    std::shared_ptr<detail::mesh_node_runtime_t> mesh;
    if (core) {
        for (const auto &registration : detail::mesh_node_runtime_t::registrations (builder))
            registration->core_context = context;
        mesh = detail::mesh_node_runtime_t::from (builder, "liveness-mesh");
        mesh->bind_serializers (serializers);
        mesh->start ().result ().value ();
    }
    detail::bind_stream_serializers (builder, serializers);
    auto runtime = detail::stream_runtime_t::from (builder);
    auto provider = services.build_provider ();
    auto listeners = std::make_shared<rt::listener_status_registry_t> ();
    auto jobs =
      std::make_shared<rt::application_job_queue_t> (rt::application_job_queue_configuration_t{});
    auto monitoring = fw::test::runtime_failure_monitoring ();
    auto capacity = std::make_shared<rt::host_capacity_runtime_t> (
      context, jobs, std::nullopt, std::nullopt, fw::core_hwm_profile_t::balanced, monitoring);
    session_callbacks_t session;
    rt::stream_host_service_t host (
      monitoring->runtime_failures, runtime, runtime.snapshots (),
      {{"probe",
        [&] (fw::service_provider_t &) -> fw::packet_stream_session_t & { return session; }}},
      options.session_replacement_callback_timeout (), mesh, {}, listeners, jobs, capacity);
    host.bind_monitoring (monitoring);
    host.start (provider).result ().value ();
    const auto endpoint =
      fw::tests::listener_endpoint (*listeners, fw::listener_kind_t::stream, "liveness");
    test (host, runtime, endpoint, session, *jobs, metrics);
}
}
