/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/telemetry_observation.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "../support/runtime_metric_contract.hpp"
#include "../support/host_readiness.hpp"

#include <zlink/framework.hpp>
#include <zlink/stream_connector.hpp>

#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/diagnostics/listener_status_registry.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/streams/stream_host_service.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include "runtime/streams/session_liveness.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include <boost/beast/websocket.hpp>
#include "../support/loopback_tcp_endpoint.hpp"


#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef ZLINK_FRAMEWORK_STREAM_TEST_WITH_OPENSSL
#include <boost/asio/connect.hpp>
#include <boost/asio/ssl.hpp>
#endif
#include "../support/stream_framework_cases.hpp"
#include "../support/stream_framework_native_fixture.hpp"
#include "../support/stream_framework_shutdown_session_fixture.hpp"

namespace zlink::framework::test::stream_framework
{
namespace
{
using zlink::framework::tests::endpoint_port;
using zlink::framework::tests::listener_endpoint;
using zlink::framework::tests::loopback_tcp_any_port;

/* Core error-frame close regression support: a session that only counts its
 * lifecycle; the rejected packet never reaches on_packet. */
class core_error_close_control_t final
{
  public:
    void record_connected () { record (_connected); }
    void record_disconnected () { record (_disconnected); }

    bool wait_connected (std::size_t count) { return wait_for (_connected, count); }
    bool wait_disconnected (std::size_t count) { return wait_for (_disconnected, count); }

    std::size_t connected () const
    {
        const std::lock_guard lock (_mutex);
        return _connected;
    }

    std::size_t disconnected () const
    {
        const std::lock_guard lock (_mutex);
        return _disconnected;
    }

  private:
    void record (std::size_t &counter)
    {
        {
            const std::lock_guard lock (_mutex);
            ++counter;
        }
        _changed.notify_all ();
    }

    bool wait_for (const std::size_t &counter, std::size_t count)
    {
        std::unique_lock lock (_mutex);
        return _changed.wait_for (lock, std::chrono::seconds (5), [&] { return counter >= count; });
    }

    mutable std::mutex _mutex;
    std::condition_variable _changed;
    std::size_t _connected = 0;
    std::size_t _disconnected = 0;
};

class core_error_close_session_t final : public zlink::framework::packet_stream_session_t
{
  public:
    explicit core_error_close_session_t (std::shared_ptr<core_error_close_control_t> control) :
        _control (std::move (control))
    {
    }

    zlink::framework::task_t<void> on_connected (zlink::framework::stream_t &) override
    {
        _control->record_connected ();
        co_return;
    }

    zlink::framework::task_t<void> on_disconnected (zlink::framework::stream_t &) override
    {
        _control->record_disconnected ();
        co_return;
    }

    zlink::framework::task_t<void> on_error (zlink::framework::stream_t &,
                                             const zlink::framework::stream_error_t &) override
    {
        co_return;
    }

    zlink::framework::task_t<void> on_packet (zlink::framework::stream_t &,
                                              const zlink::framework::session_message_context_t &,
                                              const zlink::framework::message_t &) override
    {
        co_return;
    }

  private:
    std::shared_ptr<core_error_close_control_t> _control;
};

/* Parses the collected raw STREAM bytes and reports whether an error frame
 * with the given request sequence appears. Control frames (heartbeats) are
 * skipped. */
bool contains_error_frame (const zlink::framework::detail::stream_runtime_t &runtime,
                           const std::vector<std::uint8_t> &bytes,
                           std::uint64_t request_seq)
{
    std::size_t offset = 0;
    while (bytes.size () - offset >= 6) {
        const auto header_size = (static_cast<std::size_t> (bytes[offset]) << 8)
                                 | static_cast<std::size_t> (bytes[offset + 1]);
        const auto payload_size = (static_cast<std::size_t> (bytes[offset + 2]) << 24)
                                  | (static_cast<std::size_t> (bytes[offset + 3]) << 16)
                                  | (static_cast<std::size_t> (bytes[offset + 4]) << 8)
                                  | static_cast<std::size_t> (bytes[offset + 5]);
        const auto frame_size = 6 + header_size + payload_size;
        if (bytes.size () - offset < frame_size) {
            return false;
        }
        const std::vector<std::uint8_t> header_bytes (
          bytes.begin () + static_cast<std::ptrdiff_t> (offset + 6),
          bytes.begin () + static_cast<std::ptrdiff_t> (offset + 6 + header_size));
        const auto decoded = runtime.decode_header (header_bytes);
        if (decoded
            && decoded.value ().kind () == zlink::framework::detail::stream_message_kind_t::error
            && decoded.value ().request_seq () == request_seq) {
            return true;
        }
        offset += frame_size;
    }
    return false;
}
} // namespace

int stream_core_shutdown_cases (metric_test::provider_t &metric_provider)
{
    using zlink::framework::framework_error_kind_t;
    using zlink::framework::stream_codec_t;
    using zlink::framework::detail::stream_header_flags_t;
    using zlink::framework::detail::stream_message_kind_t;

    /* A Core STREAM listener owns a poller that is stopped from the host
     * lifecycle thread. Closing the Core socket is not itself a portable
     * cross-thread poller wake-up, so stop must still join the listener within
     * the bounded poll interval. */
    zlink::framework::service_collection_t core_services;
    zlink::framework::handler_registry_t core_handlers;
    zlink::framework::serializer_registry_t core_serializers;
    zlink::framework::zlink_builder_t core_zlink =
      zlink::framework::test::runtime_failure_builder ();
    zlink::framework::detail::zlink_builder_access_t::bind_shared_core_context (
      core_zlink, std::make_shared<zlink::context_t> ());
    core_services.add_singleton<zlink::framework::detail::actor_gateway_runtime_t> ();
    core_services.add_factory<zlink::framework::session_actor_manager_t> (
      [] (zlink::framework::service_provider_t &provider) {
          return std::make_unique<zlink::framework::session_actor_manager_t> (
            provider.get_required<zlink::framework::detail::actor_gateway_runtime_t> ().manager ());
      },
      zlink::framework::service_lifetime_t::scoped);
    zlink::framework::zlink_framework_options_t core_options (core_services, core_handlers,
                                                              core_serializers, core_zlink);
    core_options.add_route_mesh ("core-stream-mesh")
      .set_routing_id (zlink::routing_id_t::from ("core-stream-node"))
      .listen (loopback_tcp_any_port);
    core_options.add_stream_node ("core-stream")
      .bind (loopback_tcp_any_port)
      .register_session ("core-session");
    core_options.apply ();
    for (const auto &registration :
         zlink::framework::detail::mesh_node_runtime_t::registrations (core_zlink)) {
        registration->core_context =
          zlink::framework::detail::zlink_builder_access_t::shared_core_context (core_zlink);
    }
    auto shutdown_session_control = std::make_shared<shutdown_session_control_t> ();
    core_services.add_factory<shutdown_failure_session_t> (
      [shutdown_session_control] (zlink::framework::service_provider_t &) {
          return std::make_unique<shutdown_failure_session_t> (shutdown_session_control);
      },
      zlink::framework::service_lifetime_t::scoped);
    auto core_provider = core_services.build_provider ();
    auto core_mesh =
      zlink::framework::detail::mesh_node_runtime_t::from (core_zlink, "core-stream-mesh");
    if (!core_mesh) {
        return 38;
    }
    core_mesh->bind_serializers (core_serializers);
    core_mesh->start ().result ().value ();
    auto core_stream_runtime = zlink::framework::detail::stream_runtime_t::from (core_zlink);
    auto core_listeners =
      std::make_shared<zlink::framework::runtime::listener_status_registry_t> ();
    zlink::framework::runtime::stream_host_service_t core_host (
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> (),
      core_stream_runtime, core_stream_runtime.snapshots (),
      {{"core-session",
        [] (zlink::framework::service_provider_t &provider)
          -> zlink::framework::packet_stream_session_t & {
            return provider.get_required<shutdown_failure_session_t> ();
        }}},
      std::chrono::milliseconds{30'000}, core_mesh, {}, core_listeners);
    core_host.start (core_provider);
    const auto core_stream_endpoint =
      core_listeners->find (zlink::framework::listener_kind_t::stream, "core-stream")->endpoint;

    zlink::stream_connector::connector_options_t core_connector_options;
    core_connector_options.endpoint =
      listener_endpoint (*core_listeners, zlink::framework::listener_kind_t::stream, "core-stream");
    core_connector_options.connect_timeout = std::chrono::seconds (2);
    core_connector_options.reconnect.enabled = false;
    auto core_connector =
      zlink::stream_connector::connector_factory_t::create (core_connector_options);
    if (!core_connector.connect () || !shutdown_session_control->wait_connected ()) {
        core_host.stop ();
        core_mesh->stop ();
        return 292;
    }
    std::optional<zlink::stream_connector::result_t<std::vector<std::uint8_t>>>
      shutdown_request_result;
    std::thread shutdown_request_thread ([&] {
        shutdown_request_result =
          core_connector
            .request (zlink::stream_connector::packet_t{
              .name = "shutdown.blocked",
              .codec = zlink::stream_connector::codec_t::raw,
              .payload = zlink::message_t::from ("shutdown-payload").to_bytes ()})
            .timeout (std::chrono::seconds (3))
            .submit<std::vector<std::uint8_t>> ();
    });
    if (!shutdown_session_control->wait_packet_entered ()) {
        (void) core_connector.close ();
        shutdown_request_thread.join ();
        core_host.stop ();
        core_mesh->stop ();
        return 293;
    }

    std::promise<void> core_stop_entered_source;
    auto core_stop_entered = core_stop_entered_source.get_future ();
    std::promise<void> core_stop_completed_source;
    auto core_stop_completed = core_stop_completed_source.get_future ();
    const auto core_stop_started = std::chrono::steady_clock::now ();
    std::thread core_stop_thread ([&] {
        core_stop_entered_source.set_value ();
        core_host.stop ();
        core_stop_completed_source.set_value ();
    });
    core_stop_entered.wait ();
    const bool stop_overtook_packet =
      core_stop_completed.wait_for (std::chrono::milliseconds (100)) == std::future_status::ready;
    shutdown_session_control->release ();
    const bool core_stop_finished =
      core_stop_completed.wait_for (std::chrono::seconds (2)) == std::future_status::ready;
    core_stop_thread.join ();
    const auto core_stop_elapsed = std::chrono::duration_cast<std::chrono::milliseconds> (
      std::chrono::steady_clock::now () - core_stop_started);
    shutdown_request_thread.join ();
    (void) core_connector.close ();
    core_mesh->stop ();
    if (stop_overtook_packet || !core_stop_finished
        || core_stop_elapsed > std::chrono::seconds (2)) {
        return 294;
    }
    if (shutdown_session_control->connected () != 1) {
        return 295;
    }
    if (shutdown_session_control->packet_entered () != 1) {
        return 296;
    }
    if (shutdown_session_control->packet_terminal () != 1) {
        return 297;
    }
    if (shutdown_session_control->disconnected () != 1) {
        return 298;
    }
    if (shutdown_session_control->destroyed () != 1) {
        return 299;
    }
    if (shutdown_session_control->lifecycle ()
        != std::vector<std::string>{"connected", "packet-entered", "packet-terminal",
                                    "disconnected", "destroyed"}) {
        return 300;
    }

    /* Core error-frame close regression (stream_host_service self-deadlock
     * removal): a synchronously rejected Core request must (a) survive a
     * pre-suspension throw in the error-frame write without deadlocking on
     * the session gate, (b) flush the error frame to the peer before the
     * protocol close ends the stream, and (c) keep the session close
     * idempotent when the deferred error_reply_failed close and the
     * protocol_error close target the same peer. */
    zlink::framework::service_collection_t error_close_services;
    zlink::framework::handler_registry_t error_close_handlers;
    zlink::framework::serializer_registry_t error_close_serializers;
    zlink::framework::zlink_builder_t error_close_zlink =
      zlink::framework::test::runtime_failure_builder ();
    zlink::framework::detail::zlink_builder_access_t::bind_shared_core_context (
      error_close_zlink, std::make_shared<zlink::context_t> ());
    error_close_services.add_singleton<zlink::framework::detail::actor_gateway_runtime_t> ();
    error_close_services.add_factory<zlink::framework::session_actor_manager_t> (
      [] (zlink::framework::service_provider_t &provider) {
          return std::make_unique<zlink::framework::session_actor_manager_t> (
            provider.get_required<zlink::framework::detail::actor_gateway_runtime_t> ().manager ());
      },
      zlink::framework::service_lifetime_t::scoped);
    zlink::framework::zlink_framework_options_t error_close_options (
      error_close_services, error_close_handlers, error_close_serializers, error_close_zlink);
    error_close_options.add_route_mesh ("error-close-mesh")
      .set_routing_id (zlink::routing_id_t::from ("error-close-node"))
      .listen (loopback_tcp_any_port);
    error_close_options.add_stream_node ("error-close-stream")
      .bind (loopback_tcp_any_port)
      .register_session ("error-close-session");
    error_close_options.apply ();
    for (const auto &registration :
         zlink::framework::detail::mesh_node_runtime_t::registrations (error_close_zlink)) {
        registration->core_context =
          zlink::framework::detail::zlink_builder_access_t::shared_core_context (error_close_zlink);
    }
    auto error_close_control = std::make_shared<core_error_close_control_t> ();
    error_close_services.add_factory<core_error_close_session_t> (
      [error_close_control] (zlink::framework::service_provider_t &) {
          return std::make_unique<core_error_close_session_t> (error_close_control);
      },
      zlink::framework::service_lifetime_t::scoped);
    auto error_close_provider = error_close_services.build_provider ();
    auto error_close_mesh =
      zlink::framework::detail::mesh_node_runtime_t::from (error_close_zlink, "error-close-mesh");
    if (!error_close_mesh) {
        return 310;
    }
    error_close_mesh->bind_serializers (error_close_serializers);
    error_close_mesh->start ().result ().value ();
    auto error_close_runtime = zlink::framework::detail::stream_runtime_t::from (error_close_zlink);
    auto error_close_listeners =
      std::make_shared<zlink::framework::runtime::listener_status_registry_t> ();
    zlink::framework::runtime::stream_host_service_t error_close_host (
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> (),
      error_close_runtime, error_close_runtime.snapshots (),
      {{"error-close-session",
        [] (zlink::framework::service_provider_t &provider)
          -> zlink::framework::packet_stream_session_t & {
            return provider.get_required<core_error_close_session_t> ();
        }}},
      std::chrono::milliseconds{30'000}, error_close_mesh, {}, error_close_listeners);
    error_close_host.start (error_close_provider);
    const auto error_close_stream_port = endpoint_port (listener_endpoint (
      *error_close_listeners, zlink::framework::listener_kind_t::stream, "error-close-stream"));

    const auto fail_error_close = [&] (int code) {
        zlink::framework::runtime::stream_host_core_test_faults ()
          .fail_core_error_frame_send.store (false, std::memory_order_release);
        error_close_host.stop ();
        error_close_mesh->stop ();
        return code;
    };

    /* A compressed request whose payload is not valid LZ4 input is rejected
     * synchronously with protocol_error while the session gate is held. */
    const auto make_bad_compressed_frame = [&] (std::uint64_t seq) {
        const zlink::framework::detail::stream_header_t bad_header (
          zlink::framework::detail::stream_message_kind_t::request, stream_codec_t::raw,
          zlink::framework::detail::stream_header_flags_t::has_request_seq
            | zlink::framework::detail::stream_header_flags_t::payload_compressed,
          seq, "error-close-probe");
        return make_native_stream_frame (error_close_runtime, bad_header,
                                         zlink::message_t::from (std::string ("this-is-not-lz4")));
    };

    /* (b) error -> close wire order: the error frame is submitted to the
     * Core socket lane before the close, so it must reach the peer before
     * the stream ends. */
    auto error_order_client = connect_loopback (error_close_stream_port);
    if (!error_order_client) {
        return fail_error_close (311);
    }
    send_native_bytes (error_order_client, make_bad_compressed_frame (7));
    const auto ordered_bytes = read_until_peer_close (error_order_client, std::chrono::seconds (5));
    close_native_client (error_order_client);
    if (!ordered_bytes) {
        return fail_error_close (312);
    }
    if (!contains_error_frame (error_close_runtime, *ordered_bytes, 7)) {
        return fail_error_close (313);
    }
    if (!error_close_control->wait_disconnected (1)) {
        return fail_error_close (314);
    }

    /* (a) pre-suspension throw in the error-frame write: the completion
     * observation runs inline in the rejecting context; the close must still
     * complete without self-deadlocking on the session gate. */
    zlink::framework::runtime::stream_host_core_test_faults ().fail_core_error_frame_send.store (
      true, std::memory_order_release);
    auto error_fault_client = connect_loopback (error_close_stream_port);
    if (!error_fault_client) {
        return fail_error_close (315);
    }
    send_native_bytes (error_fault_client, make_bad_compressed_frame (8));
    const auto faulted_bytes = read_until_peer_close (error_fault_client, std::chrono::seconds (5));
    close_native_client (error_fault_client);
    zlink::framework::runtime::stream_host_core_test_faults ().fail_core_error_frame_send.store (
      false, std::memory_order_release);
    if (!faulted_bytes) {
        /* No EOF within the deadline: the Core loop is stuck on the gate. */
        return fail_error_close (316);
    }
    if (contains_error_frame (error_close_runtime, *faulted_bytes, 8)) {
        /* The faulted write must not still deliver an error frame. */
        return fail_error_close (317);
    }
    if (!error_close_control->wait_disconnected (2)) {
        return fail_error_close (318);
    }

    /* (c) close idempotency: the deferred error_reply_failed close and the
     * protocol_error close hit the same peer; the duplicate must be a no-op,
     * so each connection observes exactly one disconnect. */
    std::this_thread::sleep_for (std::chrono::milliseconds (200));
    if (error_close_control->connected () != 2 || error_close_control->disconnected () != 2) {
        return fail_error_close (319);
    }

    /* Stop must stay bounded (no deadlocked Core loop left behind). */
    std::promise<void> error_close_stop_source;
    auto error_close_stopped = error_close_stop_source.get_future ();
    std::thread error_close_stop_thread ([&] {
        error_close_host.stop ();
        error_close_stop_source.set_value ();
    });
    const bool error_close_stop_finished =
      error_close_stopped.wait_for (std::chrono::seconds (5)) == std::future_status::ready;
    if (!error_close_stop_finished) {
        error_close_stop_thread.detach ();
        error_close_mesh->stop ();
        // The detached thread still uses error_close_host and the stop promise on this
        // stack. Leave without unwinding so nothing it touches is destroyed.
        std::cout.flush ();
        std::_Exit (320);
    }
    error_close_stop_thread.join ();
    error_close_mesh->stop ();
    const auto metric_data = metric_provider.collect ();
    try {
        metric_test::validate_metric_contract (metric_data);
        for (const auto &metric : metric_data) {
            if (metric.instrument_descriptor.name_ != "zlink.stream.connections.active")
                continue;
            for (const auto &point : metric.point_data_attr_) {
                const auto &sum =
                  opentelemetry::nostd::get<metric_test::sdk::SumPointData> (point.point_data);
                if (opentelemetry::nostd::get<double> (sum.value_) < 0)
                    throw std::runtime_error ("STREAM connection count underflow");
            }
        }
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << '\n';
        return 321;
    }
    if (const auto result =
          expired_core_disconnect_not_found_does_not_report_or_stop_next_session ();
        result != 0)
        return result;
    return 0;
}
} // namespace zlink::framework::test::stream_framework
