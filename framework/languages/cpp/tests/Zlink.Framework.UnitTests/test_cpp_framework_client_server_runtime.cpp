/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/client_server/raw_client_server_owner.hpp"
#include "runtime/client_server/client_server_failure_mapper.hpp"

#include <zlink/framework.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <future>
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
    auto observation = runtime.observe (
      "client-server-runtime-unit", 8,
      [&event_count] (const zlink::framework::observed_status_t<
                        zlink::framework::client_server_runtime_event_t> &observed) {
          assert (observed.status.channel_name == "client-server-runtime-unit");
          event_count.fetch_add (1, std::memory_order_relaxed);
      });
    const auto initial_events = event_count.load (std::memory_order_relaxed);

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
        const auto server_pump = server.pump_one (now);
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
    assert (event_count.load (std::memory_order_relaxed) > initial_events);

    observation->close ();
    app.request_stop ();
    app_thread.join ();
    assert (exit_code.load (std::memory_order_acquire) == 0);
    server.close ();
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
    verify_client_server_terminal_errors_preserve_public_boundaries ();
    verify_client_server_runtime_projection_and_observation ();
    return 0;
}
