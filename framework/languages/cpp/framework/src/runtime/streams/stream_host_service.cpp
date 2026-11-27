/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/streams/stream_host_service_internal.hpp"

#include "runtime/diagnostics/monitoring_runtime.hpp"
#include "runtime/dispatch/host_capacity_runtime.hpp"
#include "runtime/streams/stream_host_ingress.hpp"
#include "runtime/transport/listener_identity.hpp"
#include "runtime/utils/poll_interval_wait.hpp"
#include <boost/asio/post.hpp>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace zlink::framework::runtime::stream_host_detail
{

const char session_creation_failed_diagnostic[] = "STREAM session creation failed";

bool stream_uses_websocket (const stream_snapshot_t &stream)
{
    return stream.bind_endpoint.rfind ("ws://", 0) == 0;
}

bool stream_uses_tls (const stream_snapshot_t &stream)
{
    return !stream.tls_certificate_file.empty () || !stream.tls_private_key_file.empty ()
           || stream.bind_endpoint.rfind ("tls://", 0) == 0;
}

std::optional<std::string> socket_endpoint_text (const tcp::endpoint &endpoint)
{
    try {
        return endpoint.address ().to_string () + ":" + std::to_string (endpoint.port ());
    }
    catch (const boost::system::system_error &) {
        return std::nullopt;
    }
}

bool stream_trace_enabled ()
{
    const char *value = std::getenv ("ZLINK_CPP_STREAM_TRACE");
    return value != nullptr && std::string_view (value) != "0" && std::string_view (value) != "";
}

namespace
{

const char *stream_kind_name (stream_message_kind_t kind)
{
    switch (kind) {
        case stream_message_kind_t::send:
            return "send";
        case stream_message_kind_t::request:
            return "request";
        case stream_message_kind_t::response:
            return "response";
        case stream_message_kind_t::error:
            return "error";
        case stream_message_kind_t::control:
            return "control";
    }
    return "unknown";
}

} // namespace

void trace_stream_host_enabled (std::string_view stage,
                                const stream_snapshot_t &stream,
                                std::optional<stream_header_t> header,
                                std::string_view detail)
{
    std::cerr << "zlink-cpp-stream-trace side=server stage=" << stage << " stream=" << stream.name
              << " endpoint=" << stream.bind_endpoint;
    if (header) {
        std::cerr << " seq="
                  << (header->request_seq () ? std::to_string (*header->request_seq ()) : "-")
                  << " name=" << header->packet_name ()
                  << " kind=" << stream_kind_name (header->kind ());
    }
    if (!detail.empty ()) {
        std::cerr << " " << detail;
    }
    std::cerr << std::endl;
}

} // namespace zlink::framework::runtime::stream_host_detail

namespace zlink::framework::runtime
{

using stream_host_detail::socket_endpoint_text;
using stream_host_detail::stream_uses_tls;
using stream_host_detail::stream_uses_websocket;

namespace
{

struct parsed_tcp_endpoint_t
{
    std::string host;
    std::string port;
};

parsed_tcp_endpoint_t parse_tcp_endpoint (const std::string &endpoint)
{
    constexpr std::string_view prefix = "tcp://";
    if (endpoint.rfind (prefix, 0) != 0) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "STREAM host currently supports tcp endpoints only");
    }
    const auto host_start = prefix.size ();
    std::size_t separator = endpoint.rfind (':');
    std::size_t host_end = separator;
    if (endpoint.size () > host_start && endpoint[host_start] == '[') {
        const auto closing = endpoint.find (']', host_start + 1);
        if (closing == std::string::npos || closing + 1 >= endpoint.size ()
            || endpoint[closing + 1] != ':') {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "STREAM tcp endpoint must be tcp://host:port");
        }
        separator = closing + 1;
        host_end = closing;
    }
    if (separator == std::string::npos || separator <= host_start
        || separator + 1 >= endpoint.size () || host_end <= host_start) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "STREAM tcp endpoint must be tcp://host:port");
    }
    const auto bracketed = endpoint[host_start] == '[';
    return {endpoint.substr (host_start + (bracketed ? 1 : 0),
                             host_end - host_start - (bracketed ? 1 : 0)),
            endpoint.substr (separator + 1)};
}

parsed_tcp_endpoint_t parse_tls_endpoint (const std::string &endpoint)
{
    constexpr std::string_view prefix = "tls://";
    if (endpoint.rfind (prefix, 0) != 0) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "STREAM TLS host requires tls://host:port endpoint");
    }
    const auto host_start = prefix.size ();
    const auto separator = endpoint.rfind (':');
    if (separator == std::string::npos || separator <= host_start
        || separator + 1 >= endpoint.size ()) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "STREAM tls endpoint must be tls://host:port");
    }
    return {endpoint.substr (host_start, separator - host_start), endpoint.substr (separator + 1)};
}

parsed_tcp_endpoint_t parse_websocket_endpoint (const std::string &endpoint)
{
    constexpr std::string_view prefix = "ws://";
    if (endpoint.rfind (prefix, 0) != 0) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "STREAM WebSocket host requires ws://host:port endpoint");
    }
    const auto authority_start = prefix.size ();
    const auto path = endpoint.find ('/', authority_start);
    const auto authority = endpoint.substr (
      authority_start, path == std::string::npos ? std::string::npos : path - authority_start);
    const auto separator = authority.rfind (':');
    if (separator == std::string::npos || separator == 0 || separator + 1 >= authority.size ()) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "STREAM WebSocket endpoint must be ws://host:port[/path]");
    }
    return {authority.substr (0, separator), authority.substr (separator + 1)};
}

// The bind endpoint was validated with its scheme (tcp://, ws:// or tls://). Listener status
// keeps that scheme with the acceptor's actual address and port.
std::string stream_endpoint_scheme (const stream_snapshot_t &stream)
{
    return stream.bind_endpoint.substr (0, stream.bind_endpoint.find ("://") + 3);
}

parsed_tcp_endpoint_t parse_stream_endpoint (const stream_snapshot_t &stream)
{
    if (stream_uses_tls (stream)) {
        return parse_tls_endpoint (stream.bind_endpoint);
    }
    if (stream_uses_websocket (stream)) {
        return parse_websocket_endpoint (stream.bind_endpoint);
    }
    return parse_tcp_endpoint (stream.bind_endpoint);
}

void validate_stream_listener_identity (const stream_snapshot_t &stream,
                                        const std::optional<std::string> &advertise_host)
{
    const auto endpoint = parse_stream_endpoint (stream);
    try {
        (void) transport::advertised_host (endpoint.host, advertise_host, "STREAM");
    }
    catch (const std::invalid_argument &error) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     std::string (error.what ()) + ": " + stream.name);
    }
}

std::string stream_listener_advertised_endpoint (std::string bound_endpoint,
                                                 const std::optional<std::string> &advertise_host)
{
    std::string scheme = "tcp://";
    if (const auto separator = bound_endpoint.find ("://"); separator != std::string::npos) {
        scheme = bound_endpoint.substr (0, separator + 3);
        bound_endpoint.erase (0, separator + 3);
    }
    const auto port_separator = bound_endpoint.rfind (':');
    if (port_separator == std::string::npos || port_separator + 1 >= bound_endpoint.size ())
        return {};
    const auto port = bound_endpoint.substr (port_separator + 1);
    if (port == "0")
        return {};
    auto bound_host = bound_endpoint.substr (0, port_separator);
    if (bound_host.size () >= 2 && bound_host.front () == '[' && bound_host.back () == ']')
        bound_host = bound_host.substr (1, bound_host.size () - 2);
    const auto host = transport::advertised_host (bound_host, advertise_host, "STREAM");
    const auto formatted_host = transport::bracket_ipv6_host (host);
    return transport::normalize_endpoint (scheme + formatted_host + ":" + port);
}

} // namespace

stream_host_service_t::listener_t::listener_t (
  detail::stream_runtime_t runtime,
  stream_snapshot_t stream,
  std::optional<std::string> advertise_host,
  detail::stream_session_factory_t session_factory,
  service_provider_t &services,
  detail::delay_owner_t *delay_owner,
  std::atomic_bool &stop,
  std::shared_ptr<std::atomic_bool> drain_flag,
  std::stop_token shutdown_cancellation,
  std::shared_ptr<framework::detail::monitoring_runtime_state_t> monitoring,
  std::shared_ptr<runtime_failure_collector_t> runtime_failures,
  std::shared_ptr<detail::mesh_node_runtime_t> mesh_node,
  std::shared_ptr<application_job_queue_t> application_jobs,
  stream_host_ingress_t &ingress,
  std::chrono::milliseconds session_replacement_callback_timeout,
  asio::io_context &liveness_io) :
    _runtime (std::move (runtime)),
    _stream (std::move (stream)),
    _metric_transport (stream_uses_websocket (_stream) ? "ws"
                       : stream_uses_tls (_stream)     ? "tls"
                                                       : "tcp"),
    _advertise_host (std::move (advertise_host)),
    _session_factory (std::move (session_factory)),
    _services (&services),
    _delay_owner (delay_owner),
    _stop (&stop),
    _drain_flag (std::move (drain_flag)),
    _shutdown_cancellation (std::move (shutdown_cancellation)),
    _monitoring (std::move (monitoring)),
    _runtime_failures (std::move (runtime_failures)),
    _mesh_node (std::move (mesh_node)),
    _application_jobs (std::move (application_jobs)),
    _ingress (ingress),
    _session_replacement_callback_timeout (session_replacement_callback_timeout),
    _liveness_io (liveness_io),
    _acceptor (_io),
    _accept_retry_timer (_io)
{
    if (_monitoring && _monitoring->metric_meter) {
        auto &meter = *_monitoring->metric_meter;
        _opened_metric =
          meter.CreateDoubleCounter ("zlink.stream.connections.opened", "", "{connection}");
        _active_metric =
          meter.CreateDoubleUpDownCounter ("zlink.stream.connections.active", "", "{connection}");
        _closed_metric =
          meter.CreateDoubleCounter ("zlink.stream.connections.closed", "", "{connection}");
        if (_active_metric)
            _active_metric->Add (0, {{"transport", _metric_transport}});
    }
}

void stream_host_service_t::listener_t::run ()
{
    validate_stream_listener_identity (_stream, _advertise_host);
    _receive_scheduler.start ();
    if (_mesh_node && !stream_uses_tls (_stream) && !stream_uses_websocket (_stream)) {
        run_core_stream ();
        return;
    }
    if (stream_uses_tls (_stream)) {
        configure_tls_context ();
    }
    auto endpoint = parse_stream_endpoint (_stream);
    tcp::resolver resolver (_io);
    boost::system::error_code error;
    const auto wildcard = endpoint.host == "*";
    const auto resolve_host = wildcard ? std::string () : endpoint.host;
    const auto resolve_flags = wildcard ? tcp::resolver::flags::passive : tcp::resolver::flags ();
    // Port `*` selects a free port as on the Core STREAM path; resolvers take only `0` for it.
    const auto resolve_port = endpoint.port == "*" ? std::string ("0") : endpoint.port;
    const auto endpoints = resolver.resolve (resolve_host, resolve_port, resolve_flags, error);
    if (error || endpoints.begin () == endpoints.end ()) {
        throw framework_exception_t (framework_error_kind_t::internal_failure,
                                     "STREAM endpoint address resolution failed: "
                                       + (error ? error.message () : "no addresses"));
    }
    for (const auto &candidate : endpoints) {
        _acceptor.open (candidate.endpoint ().protocol (), error);
        if (!error) {
            _acceptor.set_option (tcp::acceptor::reuse_address (true), error);
        }
        if (!error) {
            _acceptor.bind (candidate.endpoint (), error);
        }
        if (!error) {
            _acceptor.listen (asio::socket_base::max_listen_connections, error);
        }
        if (!error) {
            break;
        }
        boost::system::error_code ignored;
        _acceptor.close (ignored);
    }
    if (error) {
        throw framework_exception_t (framework_error_kind_t::internal_failure,
                                     "STREAM endpoint bind/listen failed: " + error.message ());
    }
    if (const auto endpoint = socket_endpoint_text (_acceptor.local_endpoint ()))
        _bound_endpoint = stream_endpoint_scheme (_stream) + *endpoint;
    start_boost_accept ();
    mark_started ();
    _io.run ();
}

void stream_host_service_t::listener_t::run_guarded () noexcept
{
    try {
        run ();
    }
    catch (const std::exception &error) {
        mark_start_failed (error.what ());
        _runtime_failures->report (std::current_exception ());
    }
    catch (...) {
        mark_start_failed ("STREAM listener failed with an unknown exception: " + _stream.name);
        _runtime_failures->report (std::current_exception ());
    }
}

void stream_host_service_t::listener_t::wait_started ()
{
    std::unique_lock<std::mutex> lock (_ready_mutex);
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (30);
    if (!_ready_cv.wait_until (lock, deadline, [&] { return _started || _start_failed; })) {
        throw framework_exception_t (framework_error_kind_t::internal_failure,
                                     "STREAM listener did not become ready: " + _stream.name);
    }
    if (_start_failed) {
        throw framework_exception_t (framework_error_kind_t::internal_failure,
                                     _start_error.empty ()
                                       ? "STREAM listener failed to start: " + _stream.name
                                       : _start_error);
    }
}

std::string stream_host_service_t::listener_t::listener_endpoint () const
{
    std::lock_guard lock (_ready_mutex);
    return stream_listener_advertised_endpoint (_bound_endpoint, _advertise_host);
}

const std::string &stream_host_service_t::listener_t::name () const noexcept
{
    return _stream.name;
}

void stream_host_service_t::listener_t::request_stop () noexcept
{
    _core_wake_timer.signal ();
    _receive_scheduler.request_stop ();
    asio::post (_io, [this] {
        boost::system::error_code ignored;
        _acceptor.close (ignored);
        try {
            _accept_retry_timer.cancel ();
        }
        catch (const boost::system::system_error &) {
        }
    });
}

void stream_host_service_t::listener_t::stop_connections () noexcept
{
    trace_stream_host ("stop-connections-begin", _stream);
    std::vector<std::function<void ()>> cancellers;
    {
        const std::lock_guard<std::mutex> lock (_sockets_mutex);
        cancellers.reserve (_sockets.size ());
        for (const auto &entry : _sockets) {
            cancellers.push_back (entry.second);
        }
    }
    for (auto &cancel : cancellers) {
        cancel ();
    }
    _receive_scheduler.stop ();
    std::vector<std::thread> workers;
    {
        const std::lock_guard<std::mutex> lock (_workers_mutex);
        workers.swap (_workers);
    }
    trace_stream_host ("stop-connections-join-workers", _stream);
    for (auto &worker : workers) {
        if (worker.joinable ())
            worker.join ();
    }
    trace_stream_host ("stop-connections-end", _stream);
}

void stream_host_service_t::listener_t::mark_started ()
{
    {
        const std::lock_guard<std::mutex> lock (_ready_mutex);
        _started = true;
    }
    _ready_cv.notify_all ();
}

void stream_host_service_t::listener_t::mark_start_failed (std::string message)
{
    {
        const std::lock_guard<std::mutex> lock (_ready_mutex);
        // Readiness completes startup; later worker errors belong to the runtime collector.
        if (_started)
            return;
        _start_failed = true;
        _start_error = std::move (message);
    }
    _ready_cv.notify_all ();
}

#ifdef ZLINK_FRAMEWORK_TEST_CREATION_PROBE
std::size_t stream_host_service_test_access_t::active_session_count (stream_host_service_t &service)
{
    return service._listeners.at (0)->active_session_count ();
}

std::function<void ()>
stream_host_service_test_access_t::observe_shutdown (stream_host_service_t &service,
                                                     stream_host_shutdown_test_hooks_t hooks)
{
    auto listener = service._listeners.at (0);
    listener->_active_streams_lane
      .run ([listener, hooks = std::move (hooks)] () mutable {
          listener->_shutdown_test_hooks = std::move (hooks);
      })
      .get ();
    return [listener] {
        listener->force_close_sessions (stream_close_reason_t::server_drain, "shutdown race test");
    };
}
#endif

stream_host_service_t::stream_host_service_t (
  std::shared_ptr<runtime_failure_collector_t> failures,
  detail::stream_runtime_t runtime,
  std::vector<stream_snapshot_t> streams,
  std::map<std::string, detail::stream_session_factory_t> session_factories,
  std::chrono::milliseconds session_replacement_callback_timeout,
  std::shared_ptr<detail::mesh_node_runtime_t> mesh_node,
  std::map<std::string, std::optional<std::string>> advertise_hosts,
  std::shared_ptr<listener_status_registry_t> listener_statuses,
  std::shared_ptr<application_job_queue_t> application_jobs,
  std::shared_ptr<host_capacity_runtime_t> host_capacity) :
    hosted_service_lifecycle_t (std::move (failures)),
    _runtime (std::move (runtime)),
    _streams (std::move (streams)),
    _session_factories (std::move (session_factories)),
    _session_replacement_callback_timeout (session_replacement_callback_timeout),
    _advertise_hosts (std::move (advertise_hosts)),
    _listener_statuses (std::move (listener_statuses)),
    _mesh_node (std::move (mesh_node)),
    _application_jobs (
      application_jobs
        ? std::move (application_jobs)
        : std::make_shared<application_job_queue_t> (application_job_queue_configuration_t{
            application_job_queue_profile_t::balanced,
            static_cast<std::uint32_t> (std::numeric_limits<std::int32_t>::max ()), 1,
            static_cast<std::uint32_t> (std::numeric_limits<std::int32_t>::max ())})),
    _ingress (std::make_shared<stream_host_ingress_t> (_streams))
{
    if (host_capacity) {
        host_capacity->bind_authentication_admission_reset ([ingress = std::weak_ptr (_ingress)] {
            if (auto owner = ingress.lock ())
                owner->reset_metrics ();
        });
    }
}

stream_host_service_t::~stream_host_service_t ()
{
    stop ();
}

task_t<void> stream_host_service_t::start (service_provider_t &services)
{
    _services = &services;
    _stop.store (false, std::memory_order_release);
    try {
        if (_monitoring)
            _ingress->connect_metrics (_monitoring->metric_provider);
        for (const auto &stream : _streams) {
            auto factory = _session_factories.find (stream.packet_session_name);
            if (factory == _session_factories.end ()) {
                continue;
            }
            const auto advertise = _advertise_hosts.find (stream.name);
            const auto advertise_host = advertise == _advertise_hosts.end ()
                                          ? std::optional<std::string>{}
                                          : advertise->second;
            validate_stream_listener_identity (stream, advertise_host);
            auto listener = std::make_shared<listener_t> (
              _runtime, stream, advertise_host, factory->second, services, _delay_owner, _stop,
              _drain_flag, _shutdown_cancellation, _monitoring, _runtime_failures, _mesh_node,
              _application_jobs, *_ingress, _session_replacement_callback_timeout, _liveness_io);
            auto *raw = listener.get ();
            _listeners.push_back (std::move (listener));
            _threads.emplace_back ([raw] { raw->run_guarded (); });
            raw->wait_started ();
            if (_listener_statuses)
                _listener_statuses->update (listener_kind_t::stream, stream.name,
                                            raw->listener_endpoint ());
        }
        if (!_listeners.empty ()) {
            _liveness_io.restart ();
            _liveness_thread = std::thread ([this] {
                const auto keep_alive = asio::make_work_guard (_liveness_io);
                try {
                    _liveness_io.run ();
                }
                catch (...) {
                    detail::dispatch_error_reporter_t (_runtime.dispatch_options_ref ())
                      .report_lazy ([] {
                          message_dispatch_error_event_t error{};
                          error.surface = dispatch_error_surface_t::stream_session;
                          error.message_kind = dispatch_message_kind_t::control;
                          error.reason = dispatch_error_reason_t::handler_exception;
                          error.action = dispatch_error_action_t::drop;
                          error.exception = std::current_exception ();
                          return error;
                      });
                    _runtime_failures->report (std::current_exception ());
                    request_stop ();
                }
            });
        }
    }
    catch (...) {
        stop ();
        throw;
    }
    return task_t<void> (result_t<void>::success ());
}

void stream_host_service_t::notify_sessions_closing (stream_close_reason_t reason,
                                                     std::string_view diagnostic) noexcept
{
    for (const auto &listener : _listeners) {
        if (listener) {
            try {
                listener->notify_sessions_closing (reason, diagnostic);
            }
            catch (...) {
                _runtime_failures->report (std::current_exception ());
            }
        }
    }
}

void stream_host_service_t::force_close_sessions (stream_close_reason_t reason,
                                                  std::string_view diagnostic) noexcept
{
    for (const auto &listener : _listeners) {
        if (listener) {
            try {
                listener->force_close_sessions (reason, diagnostic);
            }
            catch (...) {
                _runtime_failures->report (std::current_exception ());
            }
        }
    }
}

bool stream_host_service_t::drain_sessions_until (
  std::chrono::steady_clock::time_point deadline) noexcept
{
    for (const auto &listener : _listeners) {
        if (!listener)
            continue;
        try {
            listener->begin_drain_sessions ();
        }
        catch (...) {
            _runtime_failures->report (std::current_exception ());
            return false;
        }
    }
    while (std::chrono::steady_clock::now () < deadline) {
        bool drained = true;
        for (const auto &listener : _listeners) {
            if (listener && listener->active_session_count () != 0) {
                drained = false;
                break;
            }
        }
        if (drained)
            return true;
        zlink::framework::runtime::wait_poll_interval (std::chrono::milliseconds (10));
    }
    return std::all_of (_listeners.begin (), _listeners.end (), [] (const auto &listener) {
        return !listener || listener->active_session_count () == 0;
    });
}

void stream_host_service_t::request_stop () noexcept
{
    _stop.store (true, std::memory_order_release);
    _liveness_io.stop ();
    if (_application_jobs)
        _application_jobs->stop ();
    for (auto &listener : _listeners) {
        listener->request_stop ();
    }
}

void stream_host_service_t::stop () noexcept
{
    request_stop ();
    if (_liveness_thread.joinable ())
        _runtime_failures->capture ([&] { _liveness_thread.join (); });
    for (auto &listener : _listeners)
        _runtime_failures->capture ([&] { listener->stop_connections (); });
    for (auto &thread : _threads)
        if (thread.joinable ())
            _runtime_failures->capture ([&] { thread.join (); });
    std::erase_if (_threads, [] (auto &thread) { return !thread.joinable (); });
    std::erase_if (_listeners, [&] (auto &listener) {
        const bool connections_closed =
          _runtime_failures->capture ([&] { listener->stop_connections (); });
        const bool resources_closed =
          _runtime_failures->capture ([&] { listener->close_core_resources (); });
        if (_listener_statuses)
            _runtime_failures->capture (
              [&] { _listener_statuses->remove (listener_kind_t::stream, listener->name ()); });
        return connections_closed && resources_closed;
    });
    _services = nullptr;
}

} // namespace zlink::framework::runtime
