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
#include "../support/stream_framework_session_fixture.hpp"
#include "../support/stream_framework_shutdown_session_fixture.hpp"

namespace zlink::framework::test::stream_framework
{
namespace
{
using zlink::framework::tests::endpoint_port;
using zlink::framework::tests::listener_endpoint;
using zlink::framework::tests::loopback_tcp_any_port;

double stream_metric_sum (metric_test::provider_t &provider,
                          std::string_view name,
                          std::string_view close_reason = {})
{
    const auto data = provider.collect ();
    metric_test::validate_metric_contract (data);
    double total = 0;
    bool found = false;
    for (const auto &metric : data) {
        if (metric.instrument_descriptor.name_ != name)
            continue;
        found = true;
        for (const auto &point : metric.point_data_attr_) {
            if (!close_reason.empty ()
                && opentelemetry::nostd::get<std::string> (point.attributes.at ("close_reason"))
                     != close_reason)
                continue;
            const auto &sum =
              opentelemetry::nostd::get<metric_test::sdk::SumPointData> (point.point_data);
            total += opentelemetry::nostd::get<double> (sum.value_);
        }
    }
    if (!found && name == "zlink.stream.connections.active")
        throw std::runtime_error ("connection metric instrument was not registered");
    return total;
}

const char *expected_close_reason (connection_close_case_t close_case)
{
    switch (close_case) {
        case connection_close_case_t::connected_rejected:
            return "transport_error";
        case connection_close_case_t::client_close:
        case connection_close_case_t::disconnect_rejected:
            return "client_close";
        case connection_close_case_t::server_stop:
            return "server_shutdown";
        case connection_close_case_t::heartbeat_timeout:
            return "heartbeat_timeout";
    }
    throw std::logic_error ("unknown close case");
}

std::optional<std::uint32_t>
observed_disconnect_reason (const zlink::framework::stream_error_t &error)
{
    const auto reason = error.disconnect_reason ();
    return reason ? std::optional<std::uint32_t> (static_cast<std::uint32_t> (*reason))
                  : std::nullopt;
}

class disconnect_reason_session_t final : public zlink::framework::packet_stream_session_t
{
  public:
    disconnect_reason_session_t (
      std::shared_ptr<shutdown_session_control_t> control,
      std::shared_ptr<std::vector<zlink::framework::stream_error_t>> errors) :
        _control (std::move (control)), _errors (std::move (errors))
    {
    }
    ~disconnect_reason_session_t () override { _control->record_destroyed (); }
    zlink::framework::task_t<void> on_connected (zlink::framework::stream_t &) override
    {
        _control->record_connected ();
        co_return;
    }
    zlink::framework::task_t<void> on_error (zlink::framework::stream_t &,
                                             const zlink::framework::stream_error_t &error) override
    {
        {
            const std::lock_guard lock (_errors_mutex);
            _errors->push_back (error);
        }
        _control->record_packet_entered ();
        co_await _control->wait_for_release ();
        _error_completed = true;
    }
    zlink::framework::task_t<void> on_disconnected (zlink::framework::stream_t &) override
    {
        if (!_error_completed)
            throw std::runtime_error ("disconnect callback preceded error completion");
        _control->record_disconnected ();
        co_return;
    }

  private:
    std::shared_ptr<shutdown_session_control_t> _control;
    std::shared_ptr<std::vector<zlink::framework::stream_error_t>> _errors;
    bool _error_completed = false;
    static inline std::mutex _errors_mutex;
};
} // namespace

int core_disconnect_reason_contract (metric_test::provider_t &metrics)
{
    using namespace zlink::framework;
    detail::configure_stream_dispatch_executor ();
    struct dispatch_executor_guard_t
    {
        ~dispatch_executor_guard_t () { detail::shutdown_stream_dispatch_executor (); }
    } dispatch_executor_guard;
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
    auto control = std::make_shared<shutdown_session_control_t> ();
    auto errors = std::make_shared<std::vector<stream_error_t>> ();
    services.add_factory<disconnect_reason_session_t> (
      [control, errors] (service_provider_t &) {
          return std::make_unique<disconnect_reason_session_t> (control, errors);
      },
      service_lifetime_t::scoped);
    zlink_framework_options_t options (services, handlers, serializers, builder);
    options.add_route_mesh ("expired-core-mesh")
      .set_routing_id (zlink::routing_id_t::from ("expired-core-node"))
      .listen (loopback_tcp_any_port);
    options.add_stream_node ("expired-core-stream")
      .bind (loopback_tcp_any_port)
      .register_session ("expired-core-session");
    options.apply ();
    for (const auto &registration : detail::mesh_node_runtime_t::registrations (builder))
        registration->core_context = detail::zlink_builder_access_t::shared_core_context (builder);

    std::atomic_size_t dispatch_errors{0};
    dispatch_options_t dispatch;
    detail::dispatch_options_access_t::set_dispatch_error_observer_for_tests (
      dispatch, [&dispatch_errors] (const message_dispatch_error_event_t &event) {
          if (event.surface == dispatch_error_surface_t::stream_session
              && event.message_kind == dispatch_message_kind_t::control)
              dispatch_errors.fetch_add (1, std::memory_order_relaxed);
      });
    detail::apply_dispatch_options (builder, dispatch);

    auto provider = services.build_provider ();
    auto mesh = detail::mesh_node_runtime_t::from (builder, "expired-core-mesh");
    if (!mesh)
        return 351;
    mesh->bind_serializers (serializers);
    mesh->start ().result ().value ();
    auto stream_runtime = detail::stream_runtime_t::from (builder);
    auto listeners = std::make_shared<runtime::listener_status_registry_t> ();
    runtime::stream_host_service_t host (
      std::make_shared<runtime::runtime_failure_collector_t> (), stream_runtime,
      stream_runtime.snapshots (),
      {{"expired-core-session",
        [] (service_provider_t &scope) -> packet_stream_session_t & {
            return scope.get_required<disconnect_reason_session_t> ();
        }}},
      std::chrono::milliseconds{30'000}, mesh, {}, listeners);
    struct cleanup_t
    {
        runtime::stream_host_service_t &host;
        detail::mesh_node_runtime_t &mesh;
        service_provider_t &provider;
        ~cleanup_t ()
        {
            host.stop ();
            mesh.stop ();
            provider.close ();
        }
    } cleanup{host, *mesh, provider};
    host.bind_monitoring (test::runtime_failure_monitoring ());
    host.start (provider);
    sample_session_t non_monitor_session;
    auto non_monitor_stream = stream_runtime.open_session ("expired-core-stream");
    stream_runtime
      .dispatch_error (
        non_monitor_session, non_monitor_stream,
        stream_error_t{stream_session_error_t::transport_error, "non-monitor transport error"})
      .value ();
    if (!non_monitor_session.last_error
        || observed_disconnect_reason (*non_monitor_session.last_error))
        return 359;
    if (observed_disconnect_reason (stream_error_t{})
        || observed_disconnect_reason (
          stream_error_t{stream_session_error_t::transport_error, "non-monitor transport error"}))
        return 359;
    const auto closed = stream_metric_sum (metrics, "zlink.stream.connections.closed");
    const auto client_closed =
      stream_metric_sum (metrics, "zlink.stream.connections.closed", "client_close");
    zlink::monitor_event_t event;
    event.event = zlink::monitor_event::disconnected;
    event.routing_id = zlink::routing_id_t::from (std::uint32_t{99});
    event.value = 3;
    // No session exists: a failed handshake must not create a callback target.
    runtime::stream_host_service_test_access_t::deliver_core_monitor_event (host, event);
    const std::vector<zlink::routing_id_t> rids{
      zlink::routing_id_t::from (std::uint32_t{1}), zlink::routing_id_t::from (std::uint32_t{2}),
      zlink::routing_id_t::from (std::uint32_t{3}), zlink::routing_id_t::from (std::uint32_t{4})};
    runtime::stream_host_service_test_access_t::create_core_sessions (host, rids);
    if (!control->wait_connected (4))
        return 354;
    event.routing_id = rids.front ();
    event.event = zlink::monitor_event::handshake_failed_protocol;
    runtime::stream_host_service_test_access_t::deliver_core_monitor_event (host, event);
    event.event = zlink::monitor_event::disconnected;
    const std::array<std::uint32_t, 4> reasons{0, 3, 4, 5};
    for (std::size_t i = 0; i < rids.size (); ++i) {
        event.routing_id = rids[i];
        event.value = reasons[i];
        runtime::stream_host_service_test_access_t::deliver_core_monitor_event (host, event);
        // A duplicate observation cannot emit a second error, close or metric.
        runtime::stream_host_service_test_access_t::deliver_core_monitor_event (host, event);
    }
    const bool all_errors_entered = control->wait_packet_entered (4);
    const bool premature_disconnect = control->disconnected () != 0;
    control->release ();
    const bool retired = control->wait_destroyed (4);
    host.stop ();
    if (!all_errors_entered || premature_disconnect || !retired || control->disconnected () != 4
        || errors->size () != 4) {
        std::cerr << "FAIL DISCONNECTED error callbacks=" << errors->size ()
                  << " expected=4 disconnected=" << control->disconnected () << '\n';
        return 355;
    }
    std::vector<std::uint32_t> actual;
    for (const auto &error : *errors) {
        const auto reason = observed_disconnect_reason (error);
        if (error.error () != stream_session_error_t::transport_error || !reason)
            return 356;
        actual.push_back (*reason);
    }
    std::sort (actual.begin (), actual.end ());
    if (actual != std::vector<std::uint32_t> (reasons.begin (), reasons.end ())
        || stream_metric_sum (metrics, "zlink.stream.connections.closed") != closed + 4
        || stream_metric_sum (metrics, "zlink.stream.connections.closed", "client_close")
             != client_closed + 4
        || dispatch_errors.load () != 0)
        return 357;
    const auto lifecycle = control->lifecycle ();
    // Each session's disconnected callback verifies its own error callback came first.
    if (std::count (lifecycle.begin (), lifecycle.end (), "disconnected") != 4)
        return 358;
    std::cout << "PASS DISCONNECTED once, reasons 0/3/4/5 present, error before disconnect, "
                 "closed/client_close unchanged; absent-session ignored\n";
    return 0;
}

int expired_core_disconnect_not_found_does_not_report_or_stop_next_session ()
{
    using namespace zlink::framework;
    detail::configure_stream_dispatch_executor ();
    struct dispatch_executor_guard_t
    {
        ~dispatch_executor_guard_t () { detail::shutdown_stream_dispatch_executor (); }
    } dispatch_executor_guard;
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
    auto control = std::make_shared<shutdown_session_control_t> ();
    services.add_factory<shutdown_failure_session_t> (
      [control] (service_provider_t &) {
          return std::make_unique<shutdown_failure_session_t> (control);
      },
      service_lifetime_t::scoped);
    zlink_framework_options_t options (services, handlers, serializers, builder);
    options.add_route_mesh ("expired-core-mesh")
      .set_routing_id (zlink::routing_id_t::from ("expired-core-node"))
      .listen (loopback_tcp_any_port);
    options.add_stream_node ("expired-core-stream")
      .bind (loopback_tcp_any_port)
      .register_session ("expired-core-session");
    options.apply ();
    for (const auto &registration : detail::mesh_node_runtime_t::registrations (builder))
        registration->core_context = detail::zlink_builder_access_t::shared_core_context (builder);

    std::atomic_size_t dispatch_errors{0};
    dispatch_options_t dispatch;
    detail::dispatch_options_access_t::set_dispatch_error_observer_for_tests (
      dispatch, [&dispatch_errors] (const message_dispatch_error_event_t &event) {
          if (event.surface == dispatch_error_surface_t::stream_session
              && event.message_kind == dispatch_message_kind_t::control)
              dispatch_errors.fetch_add (1, std::memory_order_relaxed);
      });
    detail::apply_dispatch_options (builder, dispatch);

    auto provider = services.build_provider ();
    auto mesh = detail::mesh_node_runtime_t::from (builder, "expired-core-mesh");
    if (!mesh)
        return 351;
    mesh->bind_serializers (serializers);
    mesh->start ().result ().value ();
    auto stream_runtime = detail::stream_runtime_t::from (builder);
    auto listeners = std::make_shared<runtime::listener_status_registry_t> ();
    runtime::stream_host_service_t host (
      std::make_shared<runtime::runtime_failure_collector_t> (), stream_runtime,
      stream_runtime.snapshots (),
      {{"expired-core-session",
        [] (service_provider_t &scope) -> packet_stream_session_t & {
            return scope.get_required<shutdown_failure_session_t> ();
        }}},
      std::chrono::milliseconds{30'000}, mesh, {}, listeners);
    struct cleanup_t
    {
        runtime::stream_host_service_t &host;
        detail::mesh_node_runtime_t &mesh;
        service_provider_t &provider;
        ~cleanup_t ()
        {
            host.stop ();
            mesh.stop ();
            provider.close ();
        }
    } cleanup{host, *mesh, provider};
    host.start (provider);
    const std::vector<zlink::routing_id_t> expired_rids{
      zlink::routing_id_t::from (std::uint32_t{1}), zlink::routing_id_t::from (std::uint32_t{2})};
    runtime::stream_host_service_test_access_t::create_core_sessions (host, expired_rids);
    if (!control->wait_connected (2)) {
        std::cerr << "FAIL test access did not create both Core STREAM sessions" << '\n';
        return 352;
    }
    runtime::stream_host_service_test_access_t::expire_core_sessions (host, expired_rids);
    const bool all_sessions_processed =
      control->wait_disconnected (2) && control->wait_destroyed (2);
    host.stop ();
    const auto reported = dispatch_errors.load (std::memory_order_relaxed);
    if (!all_sessions_processed || reported != 0) {
        std::cerr << "FAIL expired Core session cleanup completed=" << all_sessions_processed
                  << " dispatch_errors=" << reported << '\n';
        return 353;
    }
    std::cout << "PASS expired Core session NOT_FOUND is ignored and next session is processed\n";
    return 0;
}

int core_connected_rejection_cleanup (metric_test::provider_t &metrics,
                                      connection_close_case_t close_case)
{
    using namespace zlink::framework;
    const auto opened = stream_metric_sum (metrics, "zlink.stream.connections.opened");
    const auto closed = stream_metric_sum (metrics, "zlink.stream.connections.closed");
    const auto reason = expected_close_reason (close_case);
    const auto closed_with_reason =
      stream_metric_sum (metrics, "zlink.stream.connections.closed", reason);
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
    auto control = std::make_shared<shutdown_session_control_t> ();
    services.add_factory<shutdown_failure_session_t> (
      [control] (service_provider_t &) {
          return std::make_unique<shutdown_failure_session_t> (control);
      },
      service_lifetime_t::scoped);
    zlink_framework_options_t options (services, handlers, serializers, builder);
    options.add_route_mesh ("connected-rejection-mesh")
      .set_routing_id (zlink::routing_id_t::from ("connected-rejection-node"))
      .listen (loopback_tcp_any_port);
    options.add_stream_node ("connected-rejection-stream")
      .bind (loopback_tcp_any_port)
      .set_heartbeat (std::chrono::milliseconds{50}, std::chrono::milliseconds{500})
      .register_session ("connected-rejection-session");
    options.apply ();
    for (const auto &registration : detail::mesh_node_runtime_t::registrations (builder)) {
        registration->core_context = detail::zlink_builder_access_t::shared_core_context (builder);
    }

    auto rejection_source = std::make_shared<std::promise<void>> ();
    auto rejected = rejection_source->get_future ();
    dispatch_options_t dispatch;
    dispatch.message_flow (message_flow_log_mode_t::normal);
    detail::dispatch_options_access_t::set_dispatch_error_observer_for_tests (
      dispatch, [rejection_source] (const message_dispatch_error_event_t &event) {
          if (event.surface != dispatch_error_surface_t::stream_session
              || event.message_kind != dispatch_message_kind_t::control || !event.exception)
              return;
          try {
              std::rethrow_exception (event.exception);
          }
          catch (const framework_exception_t &error) {
              if (error.kind () == framework_error_kind_t::shutting_down
                  && std::string_view (error.what ()) == "stream dispatch executor is not running")
                  rejection_source->set_value ();
          }
      });
    detail::apply_dispatch_options (builder, dispatch);

    auto provider = services.build_provider ();
    auto mesh = detail::mesh_node_runtime_t::from (builder, "connected-rejection-mesh");
    if (!mesh)
        return 336;
    mesh->bind_serializers (serializers);
    auto stream_runtime = detail::stream_runtime_t::from (builder);
    auto listeners = std::make_shared<runtime::listener_status_registry_t> ();
    runtime::stream_host_service_t host (
      std::make_shared<runtime::runtime_failure_collector_t> (), stream_runtime,
      stream_runtime.snapshots (),
      {{"connected-rejection-session",
        [] (service_provider_t &scope) -> packet_stream_session_t & {
            return scope.get_required<shutdown_failure_session_t> ();
        }}},
      std::chrono::milliseconds{30'000}, mesh, {}, listeners);
    struct cleanup_t
    {
        runtime::stream_host_service_t &host;
        detail::mesh_node_runtime_t &mesh;
        service_provider_t &provider;
        ~cleanup_t ()
        {
            host.stop ();
            mesh.stop ();
            provider.close ();
            detail::configure_stream_dispatch_executor ();
        }
    } cleanup{host, *mesh, provider};
    host.bind_monitoring (test::runtime_failure_monitoring ());
    mesh->start ().result ().value ();
    host.start (provider);

    // Reject admission before any connected callback can execute.
    if (close_case == connection_close_case_t::connected_rejected)
        detail::shutdown_stream_dispatch_executor ();
    auto client = connect_loopback (endpoint_port (
      listener_endpoint (*listeners, listener_kind_t::stream, "connected-rejection-stream")));
    if (!client)
        return 337;
    if (close_case == connection_close_case_t::connected_rejected) {
        if (rejected.wait_for (std::chrono::seconds (2)) != std::future_status::ready)
            return 338;
    } else {
        if (!control->wait_connected ())
            return 343;
        if (stream_metric_sum (metrics, "zlink.stream.connections.opened") - opened != 1
            || stream_metric_sum (metrics, "zlink.stream.connections.active") != 1)
            return 345;
        if (close_case == connection_close_case_t::disconnect_rejected)
            detail::shutdown_stream_dispatch_executor ();
        if (close_case == connection_close_case_t::client_close
            || close_case == connection_close_case_t::disconnect_rejected)
            close_native_client (client, true);
        if ((close_case == connection_close_case_t::client_close
             || close_case == connection_close_case_t::heartbeat_timeout)
            && !control->wait_disconnected ())
            return 344;
    }
    if ((close_case == connection_close_case_t::connected_rejected
         || close_case == connection_close_case_t::disconnect_rejected)
        && !control->wait_destroyed ())
        return 346;
    host.stop ();
    host.stop ();
    close_native_client (client);

    // A fresh Core STREAM socket assigns its first connection RID 1. The
    // registry uses that RID's hex key and its own first connection generation.
    const runtime::stateful::stream_connection_t connection{
      zlink::routing_id_t::from (std::uint32_t{1}).to_hex (), 1, mesh->routing_id (),
      mesh->native_node ().status ().lifecycle_generation ()};
    const auto residual = mesh->native_node ().sessions ().close (connection);
    if (residual.closed || control->destroyed () != 1)
        return 339;
    const bool connected_rejected = close_case == connection_close_case_t::connected_rejected;
    const bool disconnect_rejected = close_case == connection_close_case_t::disconnect_rejected;
    if (control->connected () != (connected_rejected ? 0u : 1u)
        || control->disconnected () != (connected_rejected || disconnect_rejected ? 0u : 1u))
        return 340;
    if (stream_metric_sum (metrics, "zlink.stream.connections.opened") - opened != 1
        || stream_metric_sum (metrics, "zlink.stream.connections.closed") - closed != 1
        || stream_metric_sum (metrics, "zlink.stream.connections.active") != 0
        || stream_metric_sum (metrics, "zlink.stream.connections.closed", reason)
               - closed_with_reason
             != 1) {
        std::cerr << "FAIL connection metrics case=" << static_cast<int> (close_case) << " opened="
                  << stream_metric_sum (metrics, "zlink.stream.connections.opened") - opened
                  << " closed="
                  << stream_metric_sum (metrics, "zlink.stream.connections.closed") - closed
                  << " active=" << stream_metric_sum (metrics, "zlink.stream.connections.active")
                  << " expected_close_reason=" << reason << " matching_closed="
                  << stream_metric_sum (metrics, "zlink.stream.connections.closed", reason)
                       - closed_with_reason
                  << '\n';
        return 342;
    }
    std::cout << "PASS connection metrics case=" << static_cast<int> (close_case)
              << " opened=closed=1 active=0 close_reason=" << reason << '\n';
    return 0;
}
} // namespace zlink::framework::test::stream_framework
