/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/client_server/raw_client_server_owner.hpp"
#include "runtime/client_server/client_server_failure_mapper.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/streams/stream_runtime.hpp"

#include <zlink/framework.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace client_server = zlink::framework::runtime::client_server;
namespace protocol = zlink::framework::runtime::protocol;
using namespace std::chrono_literals;

namespace
{

std::vector<std::uint8_t> bytes (const std::string &value)
{
    return {value.begin (), value.end ()};
}

struct network_probe_message_t
{
};

struct network_probe_handler_t
{
    using message_type = network_probe_message_t;

    void handle (const network_probe_message_t &) {}
};

void verify_network_defaults_are_deferred_until_apply ()
{
    zlink::framework::service_collection_t services;
    zlink::framework::handler_registry_t handlers;
    zlink::framework::serializer_registry_t serializers;
    zlink::framework::zlink_builder_t zlink;
    zlink::framework::zlink_framework_options_t options (
      services, handlers, serializers, zlink);

    auto client_server = options.add_client_server_channel ("network-client-server");
    options.handlers ().group ("network").add_send<network_probe_handler_t> ();
    client_server.server ().listen ().add_handler_group ("network");
    options.add_fanout_channel ("network-fanout").enable_publisher ();
    auto mesh = options.add_route_mesh ("network-mesh");
    mesh.set_object_role (zlink::framework::object_role_t::none)
      .set_routing_id (zlink::routing_id_t::from ("network-mesh-node"))
      .listen ();
    options.add_stream_node ("network-stream").bind ().register_session ("network-session");

    auto &network = options.configure_network ();
    assert (network.bind_host () == "127.0.0.1");
    assert (!network.advertise_host ());
    network.set_bind_host ("127.0.0.2")
      .set_advertise_host (std::string ("network.example"));
    options.apply ();

    const auto snapshots =
      zlink::framework::detail::channel_runtime_t::from (zlink.message_bus ())
        .channel_snapshots ();
    const auto find_channel = [&snapshots] (const std::string &name) {
        return std::find_if (
          snapshots.begin (), snapshots.end (),
          [&name] (const auto &snapshot) { return snapshot.name == name; });
    };
    const auto client_server_snapshot = find_channel ("network-client-server");
    const auto fanout_snapshot = find_channel ("network-fanout");
    assert (client_server_snapshot != snapshots.end ());
    assert (fanout_snapshot != snapshots.end ());
    assert (client_server_snapshot->server.bind_endpoints.size () == 1);
    assert (client_server_snapshot->server.bind_endpoints.front ()
            == "tcp://127.0.0.2:*");
    assert (fanout_snapshot->publisher.bind_endpoints.size () == 1);
    assert (fanout_snapshot->publisher.bind_endpoints.front ()
            == "tcp://127.0.0.2:0");
    const auto stream_snapshots =
      zlink::framework::detail::stream_runtime_t::from (zlink).snapshots ();
    assert (stream_snapshots.size () == 1);
    assert (stream_snapshots.front ().bind_endpoint == "tcp://127.0.0.2:0");
    assert (zlink::framework::detail::mesh_node_runtime_t::from (
              zlink, "network-mesh")
            ->listen_endpoint ()
            == "tcp://127.0.0.2:0");
}

void verify_client_server_runtime_projection_and_observation ()
{
    protocol::client_server_server_admission_t descriptor{
      "client-server-runtime-unit",
      bytes ("client-server-runtime-unit-server"),
      17,
      1,
      100,
      zlink::framework::runtime::mesh::service_node_state_t::serving,
      "default",
      16 * 1024 * 1024,
      "tcp://127.0.0.1:0"};
    client_server::raw_client_server_server_t server ({{descriptor}});
    server.start ();
    const auto endpoint = server.descriptor ().advertised_endpoint;

    auto app = zlink::framework::app_t::create ();
    app.add_zlink_framework ([endpoint] (
                               zlink::framework::zlink_framework_options_t &options) {
        options.add_client_server_channel ("client-server-runtime-unit")
          .client ()
          .connect (endpoint);
    });
    auto provider = app.advanced ().services ().build_provider ();
    auto &runtime = provider.get_required<zlink::framework::client_server_runtime_t> ();

    const auto before = runtime.snapshot ("client-server-runtime-unit");
    assert (before.local_role == zlink::framework::client_server_role_t::client);
    assert (!before.selectable);
    assert (before.ready_server_count == 0);

    std::atomic_int event_count{0};
    std::mutex event_mutex;
    std::condition_variable event_changed;
    auto observation = runtime.observe (
      "client-server-runtime-unit", 8,
      [&event_count, &event_changed] (const zlink::framework::observed_status_t<
                                       zlink::framework::client_server_runtime_event_t> &observed) {
          assert (observed.status.channel_name == "client-server-runtime-unit");
          event_count.fetch_add (1, std::memory_order_relaxed);
          event_changed.notify_all ();
      });
    {
        std::unique_lock lock (event_mutex);
        const auto observation_deadline = std::chrono::steady_clock::now () + 5s;
        assert (event_changed.wait_until (lock, observation_deadline, [&] {
            return event_count.load (std::memory_order_relaxed) != 0;
        }));
    }
    char program[] = "client-server-runtime-unit";
    char *arguments[] = {program, nullptr};
    std::atomic_int exit_code{-1};
    std::thread app_thread ([&] {
        exit_code.store (app.run (1, arguments), std::memory_order_release);
    });

    const auto deadline = std::chrono::steady_clock::now () + 5s;
    while (!runtime.is_ready ("client-server-runtime-unit")
           && std::chrono::steady_clock::now () < deadline) {
        const auto now = std::chrono::steady_clock::now ();
        (void) server.drain_monitor_events (now);
        const auto server_pump = server.pump_one (now).result ().value ();
        assert (server_pump != client_server::client_server_pump_result_t::protocol_error);
        std::this_thread::sleep_for (1ms);
    }

    const auto after = runtime.snapshot ("client-server-runtime-unit");
    assert (after.selectable);
    assert (after.ready_server_count == 1);
    assert (after.connection_intent_count == 1);
    assert (after.servers.size () == 1);
    assert (after.servers.front ().ready);
    assert (after.servers.front ().descriptor_source == "manual");

    observation->close ();
    app.request_stop ();
    app_thread.join ();
    assert (exit_code.load (std::memory_order_acquire) == 0);
    server.close ();
}

void verify_public_listener_status_reports_bound_endpoint ()
{
    auto app = zlink::framework::app_t::create ();
    app.add_zlink_framework ([] (zlink::framework::zlink_framework_options_t &options) {
        options.handlers ().group ("listener-status").add_send<network_probe_handler_t> ();
        options.add_client_server_channel ("listener-status")
          .server ()
          .listen ()
          .add_handler_group ("listener-status");
    });

    auto provider = app.advanced ().services ().build_provider ();
    auto &runtime =
      provider.get_required<zlink::framework::framework_runtime_t> ();

    char program[] = "listener-status";
    char *arguments[] = {program, nullptr};
    std::atomic_int exit_code{-1};
    std::thread app_thread ([&] {
        exit_code.store (app.run (1, arguments), std::memory_order_release);
    });

    std::optional<zlink::framework::listener_status_t> status;
    const auto deadline = std::chrono::steady_clock::now () + 5s;
    while (std::chrono::steady_clock::now () < deadline) {
        try {
            status = runtime.listener_status (
              zlink::framework::listener_kind_t::client_server,
              "listener-status");
            break;
        } catch (const zlink::framework::framework_exception_t &) {
            std::this_thread::sleep_for (1ms);
        }
    }

    assert (status.has_value ());
    assert (status->kind == zlink::framework::listener_kind_t::client_server);
    assert (status->name == "listener-status");
    assert (status->endpoint.rfind ("tcp://", 0) == 0);
    assert (status->endpoint.find (":0") == std::string::npos);

    auto &channels =
      provider.get_required<zlink::framework::channel_client_t> ();
    const auto server_only_send =
      channels.send ("listener-status", network_probe_message_t{})
        .async ()
        .result ();
    if (server_only_send
        || server_only_send.error_kind ()
             != zlink::framework::framework_error_kind_t::not_configured) {
        throw std::runtime_error (
          "server-only ClientServer send did not return NotConfigured");
    }
    const auto server_only_request =
      channels.request_to_channel (
                "listener-status", network_probe_message_t{})
        .async<network_probe_message_t> ()
        .result ();
    if (server_only_request
        || server_only_request.error_kind ()
             != zlink::framework::framework_error_kind_t::not_configured) {
        throw std::runtime_error (
          "server-only ClientServer request did not return NotConfigured");
    }

    app.request_stop ();
    app_thread.join ();
    assert (exit_code.load (std::memory_order_acquire) == 0);
}

void verify_client_server_terminal_errors_preserve_public_boundaries ()
{
    using zlink::framework::runtime::foundation::operation_terminal_t;
    using zlink::framework::detail::boundary_error_t;
    using zlink::framework::framework_error_kind_t;
    using client_server::client_server_operation_exception;

    const auto timed_out = client_server_operation_exception (
      operation_terminal_t::timed_out, "request");
    assert (timed_out.kind () == framework_error_kind_t::deadline_exceeded);
    assert (zlink::framework::detail::boundary_state (timed_out)
            == boundary_error_t::timed_out);

    const auto cancelled = client_server_operation_exception (
      operation_terminal_t::cancelled, "request");
    assert (cancelled.kind () == framework_error_kind_t::invalid_operation);
    assert (zlink::framework::detail::boundary_state (cancelled)
            == boundary_error_t::cancelled);

    const auto disconnected = client_server_operation_exception (
      operation_terminal_t::transport_failed, "request");
    assert (disconnected.kind () == framework_error_kind_t::unavailable);
    assert (zlink::framework::detail::boundary_state (disconnected)
            == boundary_error_t::disconnected);

    const auto shutdown = client_server_operation_exception (
      operation_terminal_t::shutdown, "request");
    assert (shutdown.kind () == framework_error_kind_t::shutting_down);
    assert (zlink::framework::detail::boundary_state (shutdown)
            == boundary_error_t::shutdown);

    const auto invalid = client_server_operation_exception (
      operation_terminal_t::completed, "request");
    assert (invalid.kind () == framework_error_kind_t::internal_failure);
    assert (zlink::framework::detail::boundary_state (invalid)
            == boundary_error_t::none);
}

} // namespace

int main ()
{
    verify_network_defaults_are_deferred_until_apply ();
    verify_client_server_terminal_errors_preserve_public_boundaries ();
    verify_client_server_runtime_projection_and_observation ();
    verify_public_listener_status_reports_bound_endpoint ();
    return 0;
}
