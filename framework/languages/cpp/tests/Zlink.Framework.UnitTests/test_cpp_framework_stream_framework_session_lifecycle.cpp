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

namespace zlink::framework::test::stream_framework
{
namespace
{
using zlink::framework::tests::endpoint_port;
using zlink::framework::tests::listener_endpoint;
using zlink::framework::tests::loopback_tcp_any_port;

// Commit the native binding before disconnect; Session binding §5-2 publishes
// its slot only after the native terminal. The retirement holds the Actor callback.
class held_retirement_session_t final : public zlink::framework::packet_stream_session_t
{
  public:
    explicit held_retirement_session_t (zlink::framework::actor_ref_t actor) :
        _actor (std::move (actor))
    {
    }
    std::promise<void> connected;
    std::optional<zlink::framework::request_task_t<zlink::framework::session_actor_t>> binding;
    zlink::framework::task_t<void> on_connected (zlink::framework::stream_t &stream) override
    {
        zlink::framework::detail::session_actor_manager_access_t::bind_native (
          stream.actors (), [] (zlink::framework::actor_ref_t, std::uint64_t, std::uint16_t) {
              return zlink::framework::request_task_t<void> (
                zlink::framework::result_t<void>::success ());
          });
        binding.emplace (stream.actors ().bind (_actor).async ());
        (void) co_await *binding;
        connected.set_value ();
        co_return;
    }
    zlink::framework::task_t<void> on_disconnected (zlink::framework::stream_t &) override
    {
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
    zlink::framework::actor_ref_t _actor;
};

/* #2295 round 2: a WebSocket connection's retirement waits on the bound Actor's
 * disconnect. The listener must track it like a Core session retirement, so
 * session drain reports it and stop () waits for it before MeshNode stops. */

namespace fw = zlink::framework;
namespace connector = zlink::stream_connector;

class admission_test_session_t final : public fw::packet_stream_session_t
{
  public:
    static constexpr const char *session_name = "admission-test-session";

    fw::task_t<void> on_connected (fw::stream_t &) override { co_return; }
    fw::task_t<void> on_disconnected (fw::stream_t &) override { co_return; }
    fw::task_t<void> on_error (fw::stream_t &, const fw::stream_error_t &) override { co_return; }
    fw::task_t<void> on_packet (fw::stream_t &stream,
                                const fw::session_message_context_t &,
                                const fw::message_t &payload) override
    {
        co_await stream.reply_packet (payload).async ();
    }
};

class admission_test_readiness_t final : public fw::hosted_service_t
{
  public:
    std::promise<fw::service_provider_t *> started;

    fw::task_t<void> start (fw::service_provider_t &services) override
    {
        started.set_value (&services);
        co_return;
    }

    void stop () noexcept override {}
};
} // namespace

#ifdef ZLINK_FRAMEWORK_TEST_CREATION_PROBE
namespace
{
class core_lifecycle_session_t final : public zlink::framework::packet_stream_session_t
{
  public:
    core_lifecycle_session_t (std::promise<zlink::routing_id_t> &connected,
                              std::promise<void> &packet,
                              std::function<void (std::size_t)> destroy) :
        _connected (connected), _packet (packet), _destroy (std::move (destroy))
    {
    }

    ~core_lifecycle_session_t () override { _destroy (_disconnected); }

    zlink::framework::task_t<void> on_connected (zlink::framework::stream_t &stream) override
    {
        _connected.set_value (stream.routing_id ().value ());
        co_return;
    }

    zlink::framework::task_t<void> on_disconnected (zlink::framework::stream_t &) override
    {
        ++_disconnected;
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
        _packet.set_value ();
        co_return;
    }

  private:
    std::promise<zlink::routing_id_t> &_connected;
    std::promise<void> &_packet;
    std::size_t _disconnected = 0;
    std::function<void (std::size_t)> _destroy;
};
} // namespace

int core_session_lifecycle_boundaries (core_lifecycle_case_t scenario)
{
    using namespace zlink::framework;
    using namespace std::chrono_literals;
    using access_t = runtime::stream_host_service_test_access_t;

    service_collection_t services;
    handler_registry_t handlers;
    serializer_registry_t serializers;
    auto builder = test::runtime_failure_builder ();
    detail::zlink_builder_access_t::bind_shared_core_context (
      builder, std::make_shared<zlink::context_t> ());
    services.add_singleton<detail::actor_gateway_runtime_t> ();
    services.add_factory<session_actor_manager_t> (
      [] (service_provider_t &provider) {
          return std::make_unique<session_actor_manager_t> (
            provider.get_required<detail::actor_gateway_runtime_t> ().manager ());
      },
      service_lifetime_t::scoped);

    std::promise<void> factory_entered;
    auto entered_factory = factory_entered.get_future ();
    std::promise<void> release_factory;
    auto factory_released = release_factory.get_future ();
    std::promise<void> destructor_entered;
    auto entered_destructor = destructor_entered.get_future ();
    std::promise<void> release_destructor;
    auto destructor_released = release_destructor.get_future ();
    std::atomic_size_t factory_calls{0};
    std::atomic_size_t destroyed{0};
    std::atomic_size_t disconnected_at_destruction{0};
    std::promise<zlink::routing_id_t> connected;
    std::promise<void> packet;
    auto connected_rid = connected.get_future ();
    auto received_packet = packet.get_future ();
    services.add_factory<core_lifecycle_session_t> (
      [&] (service_provider_t &) {
          return std::make_unique<core_lifecycle_session_t> (
            connected, packet, [&] (std::size_t disconnected) {
                disconnected_at_destruction = disconnected;
                if (++destroyed == 1)
                    destructor_entered.set_value ();
                if (scenario == core_lifecycle_case_t::normal)
                    destructor_released.wait ();
            });
      },
      service_lifetime_t::scoped);

    zlink_framework_options_t options (services, handlers, serializers, builder);
    options.add_route_mesh ("lifecycle-mesh")
      .set_routing_id (zlink::routing_id_t::from ("lifecycle-node"))
      .listen (loopback_tcp_any_port);
    options.add_stream_node ("lifecycle-stream")
      .bind (loopback_tcp_any_port)
      .register_session ("lifecycle-session");
    options.apply ();
    for (const auto &registration : detail::mesh_node_runtime_t::registrations (builder))
        registration->core_context = detail::zlink_builder_access_t::shared_core_context (builder);

    dispatch_options_t dispatch;
    dispatch.message_flow (message_flow_log_mode_t::normal);
    detail::apply_dispatch_options (builder, dispatch);
    auto provider = services.build_provider ();
    auto mesh = detail::mesh_node_runtime_t::from (builder, "lifecycle-mesh");
    mesh->bind_serializers (serializers);
    auto stream_runtime = detail::stream_runtime_t::from (builder);
    auto listeners = std::make_shared<runtime::listener_status_registry_t> ();
    runtime::stream_host_service_t host (
      std::make_shared<runtime::runtime_failure_collector_t> (), stream_runtime,
      stream_runtime.snapshots (),
      {{"lifecycle-session",
        [&] (service_provider_t &scope) -> packet_stream_session_t & {
            auto &created = scope.get_required<core_lifecycle_session_t> ();
            if (++factory_calls == 1) {
                factory_entered.set_value ();
                factory_released.wait ();
            }
            if (scenario == core_lifecycle_case_t::factory_failure)
                throw std::runtime_error ("STREAM session factory failure");
            return created;
        }}},
      30s, mesh, {}, listeners);
    auto draining = std::make_shared<std::atomic_bool> (false);
    host.bind_drain_flag (draining);
    mesh->start ().result ().value ();
    host.start (provider);
    auto client = connect_loopback (
      endpoint_port (listener_endpoint (*listeners, listener_kind_t::stream, "lifecycle-stream")));

    const bool monitor_entered =
      client && entered_factory.wait_for (2s) == std::future_status::ready;
    bool passed = monitor_entered;
    if (monitor_entered) {
        // Connect alone enters the monitor path. Queue the first application
        // frame while that same RID's factory is still blocked.
        if (scenario == core_lifecycle_case_t::normal) {
            const detail::stream_header_t header (
              detail::stream_message_kind_t::send, stream_codec_t::raw,
              detail::stream_header_flags_t::none, std::nullopt, "lifecycle-packet", {});
            send_native_bytes (client, make_native_stream_frame (stream_runtime, header,
                                                                 zlink::message_t::from ("first")));
        }
        if (scenario == core_lifecycle_case_t::drain)
            draining->store (true, std::memory_order_release);

        auto registry = std::async (std::launch::async, [&] {
            return scenario == core_lifecycle_case_t::drain
                     ? host.drain_sessions_until (std::chrono::steady_clock::now () + 2s)
                     : access_t::active_session_count (host) == 0;
        });
        const bool registry_progress = registry.wait_for (200ms) == std::future_status::ready;
        release_factory.set_value ();
        passed &= registry.get () && registry_progress;
        std::cout << (registry_progress ? "PASS" : "FAIL")
                  << " registry progresses during factory, case=" << static_cast<int> (scenario)
                  << '\n';
    } else {
        release_factory.set_value ();
    }

    if (scenario == core_lifecycle_case_t::normal) {
        const bool connected_once = connected_rid.wait_for (2s) == std::future_status::ready;
        passed &= connected_once;
        if (connected_once) {
            const auto rid = connected_rid.get ();
            passed &= received_packet.wait_for (2s) == std::future_status::ready;
            auto probe_gate = access_t::probe_core_session_gate (host, rid);
            close_native_client (client, true);
            if (entered_destructor.wait_for (2s) == std::future_status::ready) {
                const bool gate_available = probe_gate ();
                const bool tracked = access_t::active_session_count (host) == 1;
                const bool drained = host.drain_sessions_until (std::chrono::steady_clock::now ());
                passed &= gate_available && tracked && !drained;
                std::cout << (gate_available ? "PASS" : "FAIL")
                          << " session gate available during scope destruction\n";
            } else {
                passed = false;
            }
        }
        release_destructor.set_value ();
    } else {
        passed &= entered_destructor.wait_for (2s) == std::future_status::ready;
    }

    host.stop ();
    close_native_client (client);
    const runtime::stateful::stream_connection_t connection{
      zlink::routing_id_t::from (std::uint32_t{1}).to_hex (), 1, mesh->routing_id (),
      mesh->native_node ().status ().lifecycle_generation ()};
    const bool residual = mesh->native_node ().sessions ().close (connection).closed;
    mesh->stop ();
    provider.close ();
    const std::size_t expected_disconnected = scenario == core_lifecycle_case_t::normal ? 1 : 0;
    const bool connected_observed =
      connected_rid.valid () && connected_rid.wait_for (0s) == std::future_status::ready;
    passed &= factory_calls == 1 && destroyed == 1 && !residual
              && disconnected_at_destruction == expected_disconnected
              && (scenario == core_lifecycle_case_t::normal || !connected_observed);
    std::cout << (passed ? "PASS" : "FAIL")
              << " Core STREAM lifecycle, case=" << static_cast<int> (scenario)
              << " factory=" << factory_calls << " destroyed=" << destroyed
              << " disconnected=" << disconnected_at_destruction << " residual=" << residual
              << " unexpected_connected=" << connected_observed << '\n';
    return passed ? 0 : 1;
}
#endif

int reader_retirement_is_tracked_until_complete ()
{
    using namespace zlink::framework;
    using namespace std::chrono_literals;
    detail::configure_stream_dispatch_executor ();
    service_collection_t services;
    handler_registry_t handlers;
    serializer_registry_t serializers;
    auto builder = test::runtime_failure_builder ();
    detail::zlink_builder_access_t::bind_shared_core_context (
      builder, std::make_shared<zlink::context_t> ());
    services.add_singleton<detail::actor_gateway_runtime_t> ();
    services.add_factory<session_actor_manager_t> (
      [] (service_provider_t &provider) {
          return std::make_unique<session_actor_manager_t> (
            provider.get_required<detail::actor_gateway_runtime_t> ().manager ());
      },
      service_lifetime_t::scoped);
    held_retirement_session_t session (detail::actor_ref_access_t::make (
      node_rid_t::from_string ("held-actor-node"), "player", "held-actor", 1));
    auto connected = session.connected.get_future ();
    zlink_framework_options_t options (services, handlers, serializers, builder);
    options.add_route_mesh ("held-mesh")
      .set_routing_id (zlink::routing_id_t::from ("held-node"))
      .listen (loopback_tcp_any_port);
    // With a MeshNode, tcp:// sessions use the Core socket path. WebSocket
    // uses the per-connection reader path under test.
    options.add_stream_node ("held-stream")
      .bind ("ws://127.0.0.1:0/held")
      .register_session ("held-session");
    options.apply ();
    for (const auto &registration : detail::mesh_node_runtime_t::registrations (builder))
        registration->core_context = detail::zlink_builder_access_t::shared_core_context (builder);
    auto provider = services.build_provider ();
    auto mesh = detail::mesh_node_runtime_t::from (builder, "held-mesh");
    mesh->bind_serializers (serializers);
    auto disconnect = std::make_shared<task_completion_source_t<void>> ();
    auto disconnect_entered = std::make_shared<std::promise<void>> ();
    provider.get_required<detail::actor_gateway_runtime_t> ().on_disconnect (
      [disconnect, disconnect_entered] (const actor_ref_t &, const auto &) {
          disconnect_entered->set_value ();
          return disconnect->task ();
      });
    mesh->start ().result ().value ();
    auto runtime = detail::stream_runtime_t::from (builder);
    auto listeners = std::make_shared<runtime::listener_status_registry_t> ();
    runtime::stream_host_service_t host (
      std::make_shared<runtime::runtime_failure_collector_t> (), runtime, runtime.snapshots (),
      {{"held-session",
        [&session] (service_provider_t &) -> packet_stream_session_t & { return session; }}},
      30s, mesh, {}, listeners);
    host.start (provider);
    boost::asio::io_context io;
    boost::beast::websocket::stream<boost::asio::ip::tcp::socket> client (io);
    client.next_layer ().connect (
      {boost::asio::ip::make_address ("127.0.0.1"),
       endpoint_port (listener_endpoint (*listeners, listener_kind_t::stream, "held-stream"))});
    client.handshake ("127.0.0.1", "/held");
    auto disconnect_started = disconnect_entered->get_future ();
    int result = 0;
    if (connected.wait_for (5s) != std::future_status::ready)
        result = 1;
    boost::system::error_code ignored;
    client.next_layer ().close (ignored);
    if (result == 0 && disconnect_started.wait_for (5s) != std::future_status::ready)
        result = 2;
    // The retirement is held on the Actor's disconnect. The deadline has
    // already passed, so this only reads whether the listener tracks it.
    if (result == 0 && host.drain_sessions_until (std::chrono::steady_clock::now ()))
        result = 3;
    disconnect->complete (result_t<void>::success ());
    host.stop ();
    mesh->stop ();
    provider.close ();
    detail::shutdown_stream_dispatch_executor ();
    return result;
}

int stream_write_terminal_during_host_shutdown (bool success)
{
    using namespace zlink::framework;
    runtime::configure_handler_coroutine_executor (1);
    runtime::install_host_context_hooks ();
    auto builder = test::runtime_failure_builder ();
    builder.stream ("write-shutdown").bind ("tcp://127.0.0.1:0").register_session ("session");
    serializer_registry_t serializers;
    detail::bind_stream_serializers (builder, serializers);
    auto streams = detail::stream_runtime_t::from (builder);
    auto stream = streams.open_session ("write-shutdown");
    task_completion_source_t<void> native_write;
    streams.attach_transport_writer (
      stream, [&] (const auto &, const auto &) { return native_write.task (); });

    std::promise<void> worker_entered, release_worker;
    auto released = release_worker.get_future ().share ();
    auto scheduler = detail::capture_runtime_native_continuation_scheduler ();
    scheduler ([&] {
        worker_entered.set_value ();
        released.wait ();
    });
    worker_entered.get_future ().wait ();

    runtime::offload_executor_t workers (1);
    auto queue = std::make_shared<runtime::serial_execution_queue_t> (workers);
    std::optional<task_t<void>> write;
    queue->run ("write", [&] {
        write.emplace (stream.write_packet (message_t::from (std::string ("body"))).async ());
    });
    native_write.complete (
      success ? result_t<void>::success ()
              : result_t<void>::failure (framework_error_kind_t::unavailable, "write failed"));
    std::thread shutdown ([] { runtime::shutdown_handler_coroutine_executor (); });
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
    while (detail::capture_runtime_native_continuation_scheduler ()
           && std::chrono::steady_clock::now () < deadline)
        std::this_thread::yield ();
    const bool sealed = !detail::capture_runtime_native_continuation_scheduler ();
    release_worker.set_value ();
    shutdown.join ();

    const bool completed = write->await_ready ();
    const bool preserved =
      completed && static_cast<bool> (write->result ()) == success
      && (success || write->result ().error_kind () == framework_error_kind_t::unavailable);
    streams.mark_disconnected (stream);
    queue->close ();
    queue->drain ();
    queue.reset ();
    workers.drain ();
    return sealed && preserved ? 0 : 1;
}

int session_creation_failure (bool websocket)
{
    using namespace std::chrono_literals;
    const std::string node_name = "admission-test-stream";
    const std::string exception_message = "application session factory rejected connection";
    std::atomic_bool fail_next{false};
    std::atomic_size_t factory_failures{0};
    std::atomic_size_t diagnostic_errors{0};
    std::atomic_size_t creation_failure_flows{0};

    auto app = fw::app_t::create ();
    app.logging ().use_provider ("admission-test", [&] (const fw::log_record_t &record) {
        std::string details = record.message;
        for (const auto &field : record.fields) {
            details += " " + field.value;
            if (field.value == "session_creation_failed")
                ++creation_failure_flows;
        }
        if (record.level == fw::log_level_t::error && details.find (node_name) != std::string::npos
            && details.find (admission_test_session_t::session_name) != std::string::npos
            && details.find (exception_message) != std::string::npos)
            ++diagnostic_errors;
    });
    app.advanced ().services ().add_factory<admission_test_session_t> (
      [&] (fw::service_provider_t &) {
          if (fail_next.exchange (false)) {
              ++factory_failures;
              throw std::runtime_error (exception_message);
          }
          return std::make_unique<admission_test_session_t> ();
      },
      fw::service_lifetime_t::scoped);
    auto &options = app.add_zlink_framework ();
    options.configure_dispatch ().message_flow (fw::message_flow_log_mode_t::normal);
    options.add_route_mesh ("admission-test-mesh")
      .set_automatic_routing_id_prefix ("admission-test")
      .listen (loopback_tcp_any_port);
    options.add_stream_node (node_name)
      .bind (websocket ? "ws://127.0.0.1:0" : "tcp://127.0.0.1:0")
      .register_session<admission_test_session_t> ();
    auto readiness = std::make_unique<admission_test_readiness_t> ();
    auto started = readiness->started.get_future ();
    app.add_hosted_service (std::move (readiness));
    int host_result = -1;
    std::thread host ([&] { host_result = app.run (0, nullptr); });
    struct host_guard_t
    {
        fw::app_t &app;
        std::thread &host;
        ~host_guard_t ()
        {
            if (host.joinable ()) {
                app.stop ();
                host.join ();
            }
        }
    } host_guard{app, host};

    if (started.wait_for (5s) != std::future_status::ready) {
        std::cerr << "FAIL STREAM admission host did not start\n";
        return 1;
    }
    auto &services = *started.get ();
    fw::test::wait_for_serving (services);
    connector::connector_options_t client_options;
    client_options.endpoint = services.get_required<fw::framework_runtime_t> ()
                                .listener_status (fw::listener_kind_t::stream, node_name)
                                .endpoint;
    client_options.dispatch_mode = connector::dispatch_mode_t::immediate;
    client_options.connect_timeout = 2s;
    client_options.reconnect.enabled = false;
    const auto echo_ok = [] (connector::connector_t &client) {
        const std::vector<std::uint8_t> request{1, 2, 3};
        const auto reply =
          client.request (connector::packet_t{.name = "admission-test-echo", .payload = request})
            .timeout (2s)
            .submit<std::vector<std::uint8_t>> ();
        return reply && reply.value () == request;
    };
    bool existing_connection_ok = false;
    bool next_connection_ok = false;
    bool transport_error = false;
    {
        auto existing = connector::connector_factory_t::create (client_options);
        if (!existing.connect () || !echo_ok (existing)) {
            std::cerr << "FAIL first healthy STREAM connection could not exchange a request\n";
            return 1;
        }
        fail_next = true;
        std::promise<std::optional<connector::close_reason_t>> disconnect;
        auto disconnected = disconnect.get_future ();
        auto rejected = connector::connector_factory_t::create (client_options);
        const auto subscription = rejected.on_disconnected (
          [&] (std::optional<connector::close_reason_t> reason) { disconnect.set_value (reason); });
        (void) rejected.connect ();
        transport_error = disconnected.wait_for (2s) == std::future_status::ready
                          && disconnected.get () == connector::close_reason_t::transport_error;
        existing_connection_ok = echo_ok (existing);
        auto next = connector::connector_factory_t::create (client_options);
        next_connection_ok = next.connect () && echo_ok (next);
    }
    app.stop ();
    host.join ();

    const bool passed = transport_error && existing_connection_ok && next_connection_ok
                        && factory_failures == 1 && diagnostic_errors == 1
                        && creation_failure_flows == 0 && host_result == 0;
    std::cout << (passed ? "PASS" : "FAIL") << " STREAM session creation failure"
              << " transport_error=" << transport_error
              << " existing_connection_ok=" << existing_connection_ok
              << " next_connection_ok=" << next_connection_ok
              << " factory_failures=" << factory_failures
              << " diagnostic_errors=" << diagnostic_errors
              << " connection_only_creation_failure_flows=" << creation_failure_flows
              << " host_result=" << host_result << '\n';
    return passed ? 0 : 1;
}
} // namespace zlink::framework::test::stream_framework
