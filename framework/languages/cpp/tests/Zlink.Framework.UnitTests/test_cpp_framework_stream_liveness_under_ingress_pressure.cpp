/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/loopback_tcp_endpoint.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "metric_test_reader.hpp"

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

namespace
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

void require (bool condition, std::string_view message)
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

void check_liveness (std::string_view transport, std::string_view scenario)
{
    std::cout << "BEGIN " << transport << ' ' << scenario << std::endl;

    const bool saturate = scenario != "unsaturated";
    const bool silent_after_release = scenario == "silent_after_release";
    const auto idle_timeout = scenario == "idle_after_release" ? 450ms : 0ms;
    const auto timeout_after_release = silent_after_release ? receive_timeout : idle_timeout;
    const auto expected_reason = idle_timeout > 0ms ? fw::stream_close_reason_t::idle_timeout
                                                    : fw::stream_close_reason_t::heartbeat_timeout;
    const auto expected_reason_label = idle_timeout > 0ms ? "idle_timeout" : "heartbeat_timeout";
    const auto duration = scenario == "below_timeout" ? 300ms
                          : scenario == "at_timeout"  ? receive_timeout
                                                      : 1500ms;
    const bool core = transport == "core";
    // An incomplete Asio frame starts the permit wait and leaves the inbound
    // clock dependent on the permit release, without a decoded frame to refresh it.
    const bool partial_ping = silent_after_release && !core;
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

    ping_only_client_t client (
      fw::tests::listener_endpoint (*listeners, fw::listener_kind_t::stream, "liveness"), runtime);
    const auto &peer = client.observation ();
    client.send_ping ();
    const auto warmup_deadline = steady_clock::now () + 750ms;
    while ((peer.pongs == 0 || session.connections == 0) && !peer.closed
           && steady_clock::now () < warmup_deadline)
        client.advance ();
    require (peer.pongs == 1 && peer.writes == 1 && session.connections == 1,
             "Warm-up ping did not establish a responding STREAM session");

    std::optional<rt::application_job_queue_t::permit_t> held_permit;
    if (saturate) {
        // Separate the last decoded ping from the start of the hold so that
        // the 1,000 ms case does not tie the old timeout with permit release.
        const auto quiet_until = steady_clock::now () + 100ms;
        while (!peer.closed && steady_clock::now () < quiet_until)
            client.advance ();
        require (!peer.closed && !peer.closing, "The session closed before the permit hold");

        const auto reservation_deadline = steady_clock::now () + 100ms;
        while (!(held_permit = jobs->try_reserve_supply ())
               && steady_clock::now () < reservation_deadline)
            client.advance ();
        require (held_permit && jobs->snapshot ().permits_in_use == 1
                   && !jobs->try_reserve_supply (),
                 "Test could not occupy the only application permit");
    }
    const auto start = steady_clock::now ();
    auto server_pings_at_wait = peer.server_pings;
    std::optional<steady_clock::time_point> reader_waiting;
    auto next_ping = start;
    const auto drive_client = [&] {
        const auto now = steady_clock::now ();
        if (now >= next_ping && (!silent_after_release || peer.writes == 1)) {
            client.send_ping (partial_ping);
            next_ping = now + probe_period;
        }
        if (peer.closed)
            std::this_thread::sleep_for (2ms);
        else
            client.advance ();
    };
    while (steady_clock::now () < start + duration) {
        if (saturate && !reader_waiting && jobs->snapshot ().capacity_waiters == 1) {
            reader_waiting = steady_clock::now ();
            server_pings_at_wait = peer.server_pings;
        }
        drive_client ();
    }
    if (saturate) {
        const auto write_deadline = steady_clock::now () + probe_period;
        while (client.write_pending () && steady_clock::now () < write_deadline)
            client.advance ();
        require (!client.write_pending (), "Client ping write was still pending at release");
    }

    const auto at_release = peer;
    if (saturate)
        require (jobs->snapshot ().permits_in_use == 1, "Test lost the permit before release");
    const auto released = steady_clock::now ();
    held_permit.reset ();
    const auto post_release_window = timeout_after_release + observation_after_release;
    while (!peer.closed && steady_clock::now () < released + post_release_window) {
        if (saturate)
            client.advance ();
        else
            drive_client ();
    }

    if (peer.closed) {
        const auto cleanup_deadline = steady_clock::now () + observation_after_release;
        while (session.disconnections == 0 && steady_clock::now () < cleanup_deadline)
            std::this_thread::sleep_for (2ms);
        require (session.disconnections == 1,
                 "Closed session did not finish its disconnect callback");
    }

    std::optional<std::string> server_reason;
    for (const auto &metric : metrics.collect ()) {
        if (metric.instrument_descriptor.name_ != "zlink.stream.connections.closed")
            continue;
        for (const auto &point : metric.point_data_attr_) {
            const auto &sum =
              opentelemetry::nostd::get<metric_test::sdk::SumPointData> (point.point_data);
            require (!server_reason && opentelemetry::nostd::get<double> (sum.value_) == 1.0,
                     "Expected a close metric for exactly one session");
            server_reason =
              opentelemetry::nostd::get<std::string> (point.attributes.at ("close_reason"));
        }
    }

    const auto elapsed = [start] (std::optional<steady_clock::time_point> time) {
        return time ? std::chrono::duration_cast<std::chrono::milliseconds> (*time - start).count ()
                    : -1;
    };
    const auto pongs_after_release = peer.pongs - at_release.pongs;
    std::cout << "OBSERVATION " << transport << ' ' << scenario
              << " target_ms=" << duration.count () << " release_ms=" << elapsed (released)
              << " first_waiter_ms=" << elapsed (reader_waiting)
              << " pings_in_window=" << at_release.writes - 1
              << " last_ping_in_window_ms=" << elapsed (at_release.last_write)
              << " pongs_in_window=" << at_release.pongs - 1
              << " pongs_after_release=" << pongs_after_release
              << " server_pings_in_window=" << at_release.server_pings - server_pings_at_wait
              << " closing_ms=" << elapsed (peer.closing) << " closed_ms=" << elapsed (peer.closed)
              << " close_after_release_ms="
              << (peer.closed ? elapsed (peer.closing.value_or (*peer.closed)) - elapsed (released)
                              : -1)
              << " close_observed_before_release=" << at_release.closed.has_value ()
              << " server_close_reason=" << server_reason.value_or ("none") << " wire_close_reason="
              << (peer.reason == expected_reason ? expected_reason_label
                  : peer.reason                  ? "unexpected"
                                                 : "none")
              << " read_error=" << peer.read_error.message ()
              << " write_error=" << peer.write_error.message () << std::endl;

    require (session.errors == 0 && session.packets == 0 && session.connections == 1,
             "Unexpected session callback during the ping-only case");
    if (saturate) {
        require (reader_waiting.has_value (), "Reader did not contend for the application permit");
        require (at_release.writes > 1 && at_release.pongs == 1,
                 "Expected successful ping writes without pongs while the permit was held");
        require (!at_release.closed && !at_release.closing,
                 "The session closed while the reader was waiting for the application permit");
        require (at_release.server_pings - server_pings_at_wait >= std::max (
                   decltype (duration / server_ping_period){1}, duration / server_ping_period - 1),
                 "The server did not keep sending heartbeat pings during the permit hold");
        const auto expected_pongs = partial_ping ? 0 : at_release.writes - at_release.pongs;
        require (pongs_after_release == expected_pongs,
                 "The server did not answer the pings queued during the permit hold");
    }

    const bool alive = !peer.closing && !peer.closed;
    if (alive) {
        require (!server_reason && !peer.write_error && pongs_after_release > 0,
                 "The session stayed open without answering pings after release");
    } else {
        require (peer.closed && server_reason == expected_reason_label,
                 "The server did not record the expected timeout reason");
        require (!peer.reason || peer.reason == expected_reason,
                 "Session-closing reported a different reason from the server metric");
        require (peer.read_error == asio::error::eof
                   || peer.read_error == asio::error::connection_reset
                   || peer.read_error == websocket::error::closed,
                 "Client did not observe an expected transport close");
    }
    if (!saturate) {
        require (alive && at_release.pongs > 10, "The unsaturated control case did not stay live");
    } else if (timeout_after_release > 0ms) {
        if (silent_after_release)
            require (peer.writes == 2, "The silent peer must write exactly once during the hold");
        require (peer.closed
                   && peer.closing.value_or (*peer.closed) >= released + timeout_after_release
                   && *peer.closed <= released + post_release_window,
                 "The session did not close one full timeout after release");
    } else {
        require (alive, "The session did not stay live through the permit hold and release");
    }
}
}

int main (int argc, char **argv)
{
    try {
        const fw::completion_owner_t completion_owner;
        detail::configure_stream_dispatch_executor ();
        struct dispatch_executor_guard_t
        {
            ~dispatch_executor_guard_t () { detail::shutdown_stream_dispatch_executor (); }
        } dispatch_executor_guard;
        require (argc == 3, "Expected a transport and a scenario");
        check_liveness (argv[1], argv[2]);
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << "FAIL " << error.what () << std::endl;
        return 1;
    }
}
