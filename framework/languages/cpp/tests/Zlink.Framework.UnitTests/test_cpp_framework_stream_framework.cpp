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

using namespace zlink::framework::test::stream_framework;

int main (int argc, char **argv)
{
    zlink::framework::completion_owner_t completion_owner;
    if (argc == 2 && std::string_view (argv[1]) == "--session-creation-failure-tcp")
        return session_creation_failure (false);
    if (argc == 2 && std::string_view (argv[1]) == "--session-creation-failure-ws")
        return session_creation_failure (true);
#ifdef ZLINK_FRAMEWORK_TEST_CREATION_PROBE
    if (argc == 2 && std::string_view (argv[1]) == "--core-session-lifecycle-boundaries") {
        zlink::framework::detail::configure_stream_dispatch_executor ();
        const auto normal = core_session_lifecycle_boundaries (core_lifecycle_case_t::normal);
        const auto drain = core_session_lifecycle_boundaries (core_lifecycle_case_t::drain);
        const auto failure =
          core_session_lifecycle_boundaries (core_lifecycle_case_t::factory_failure);
        zlink::framework::detail::shutdown_stream_dispatch_executor ();
        return normal | drain | failure;
    }
#endif
    if (argc == 2 && std::string_view (argv[1]) == "--write-terminal-shutdown") {
        if (const auto failed = stream_write_terminal_during_host_shutdown (true))
            return failed;
        return stream_write_terminal_during_host_shutdown (false);
    }
    if (argc == 2 && std::string_view (argv[1]) == "--reply-unbound")
        return reply_submission_after_unbound (false) ? 5 : reply_submission_after_unbound ();
    if (argc == 2 && std::string_view (argv[1]) == "--binding-submission-order")
        return binding_submission_order ();
    if (argc == 2 && std::string_view (argv[1]) == "--expired-core-disconnect-not-found")
        return expired_core_disconnect_not_found_does_not_report_or_stop_next_session ();
    if (argc == 2 && std::string_view (argv[1]) == "--reader-retirement-tracked")
        return reader_retirement_is_tracked_until_complete ();
    if (argc == 2 && std::string_view (argv[1]) == "--connection-registry-lifetime") {
        /* #2295: the last reference to a connection state is released after
         * MeshNode has released the host that owns the session registry. The
         * state's destructor must not use the registry. */
        using namespace zlink::framework;
        auto builder = test::runtime_failure_builder ();
        builder.stream ("lifetime-stream").bind ("tcp://127.0.0.1:0").register_session ("session");
        auto runtime = detail::stream_runtime_t::from (builder);
        auto stream = runtime.open_session ("lifetime-stream");
        service_collection_t services;
        auto provider = services.build_provider ();
        auto options = test::runtime_failure_options (runtime::mesh::raw_mesh_node_options_t{
          .descriptor = {"lifetime-mesh",
                         {'l', 'i', 'f', 'e'},
                         1,
                         1,
                         "tcp://127.0.0.1:0",
                         {},
                         runtime::mesh::service_node_state_t::preparing}});
        auto node = std::make_shared<runtime::host::public_host_runtime_t> (
          runtime::host::host_options_t{std::move (options)});
        std::weak_ptr<runtime::host::public_host_runtime_t> owner = node;
        auto retained =
          runtime::stream_host_service_test_access_t::retain_connection (provider, stream, *node);
        node.reset ();
        retained.reset ();
        return owner.expired () ? 0 : 1;
    }
#ifdef ZLINK_FRAMEWORK_STREAM_TEST_WITH_OPENSSL
    if (argc == 2 && std::string_view (argv[1]) == "--tls-bound-close")
        return transport_bound_close (true);
    if (argc == 2 && std::string_view (argv[1]) == "--tls-remote-bound-close")
        return transport_bound_close (true, true);
    if (argc == 2 && std::string_view (argv[1]) == "--ws-bound-close")
        return transport_bound_close (false);
    if (argc == 2 && std::string_view (argv[1]) == "--ws-remote-bound-close")
        return transport_bound_close (false, true);
    if (argc == 1) {
        if (const auto result = transport_bound_close (true); result != 0)
            return result;
        if (const auto result = transport_bound_close (false); result != 0)
            return result;
        if (const auto result = transport_bound_close (true, true); result != 0)
            return result;
        if (const auto result = transport_bound_close (false, true); result != 0)
            return result;
    }
#endif
    if (argc == 2 && std::string_view (argv[1]) == "--descriptor-drop")
        return descriptor_unavailable_replacement_records_drop ();
    if (descriptor_unavailable_replacement_records_drop () != 0)
        return 202;
    metric_test::provider_t metric_provider;
    using zlink::framework::framework_error_kind_t;
    using zlink::framework::stream_codec_t;
    using zlink::framework::detail::stream_header_flags_t;
    using zlink::framework::detail::stream_message_kind_t;

    if (argc == 2 && std::string_view (argv[1]) == "--disconnect-reason")
        return core_disconnect_reason_contract (metric_provider);
    if (argc == 2 && std::string_view (argv[1]) == "--core-connected-rejection-cleanup")
        return core_connected_rejection_cleanup (metric_provider);
    const bool metrics_only = argc == 2 && std::string_view (argv[1]) == "--connection-metrics";
    if (argc != 1 && !metrics_only)
        return 341;
    for (const auto close_case :
         {connection_close_case_t::connected_rejected, connection_close_case_t::client_close,
          connection_close_case_t::disconnect_rejected, connection_close_case_t::server_stop,
          connection_close_case_t::heartbeat_timeout}) {
        if (const auto result = core_connected_rejection_cleanup (metric_provider, close_case);
            result != 0)
            return result;
    }

    if (metrics_only)
        return 0;

    return stream_dispatch_cases (metric_provider);
}
