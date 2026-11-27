/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/host/app_internal.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/diagnostics/diagnostic_event_sink.hpp"
#include "runtime/diagnostics/listener_status_registry.hpp"
#include "runtime/diagnostics/monitoring_runtime.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/dispatch/host_capacity_runtime.hpp"
#include "runtime/execution/task_result.hpp"
#include "runtime/host/app_test_access.hpp"
#include "runtime/host/hosted_service_lifecycle.hpp"
#include "runtime/streams/stream_host_ingress.hpp"
#include "runtime/streams/stream_host_service.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include "runtime/utils/poll_interval_wait.hpp"
#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <typeinfo>

namespace zlink::framework::detail
{

void shutdown_handler_invocation_executor () noexcept;

runtime::hosted_service_lifecycle_t *lifecycle_of (hosted_service_t *service) noexcept
{
    return dynamic_cast<runtime::hosted_service_lifecycle_t *> (service);
}

app_state_t::app_state_t () :
    status_access (std::make_shared<app_state_access_t> ()),
    monitoring (std::make_shared<monitoring_runtime_state_t> ()),
    monitoring_builder (monitoring),
    listener_statuses (std::make_shared<runtime::listener_status_registry_t> ())
{
    status_access->state = this;
    monitoring->runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ();
    bind_zlink_monitoring (zlink, monitoring);
    monitoring->runtime_failures->bind_runtime_reporter (
      [state = std::weak_ptr (monitoring)] (std::exception_ptr failure) {
          if (auto monitoring = state.lock ())
              monitoring->diagnostics_logger.error (
                diagnostic_event_sink_t::exception_summary (failure).message);
      });
}
app_state_t::~app_state_t ()
{
    if (framework_options)
        dispatch_options_access_t::retire_telemetry (framework_options->dispatch_options ());
    delays.stop_and_join ();
    wait_relocation_cleanup ();
    if (application_job_queue)
        application_job_queue->stop ();
    if (relocation_operation.worker.joinable ())
        relocation_operation.worker.join ();
    if (termination_operation.worker.joinable ())
        termination_operation.worker.join ();
    const char *trace_value = std::getenv ("ZLINK_CPP_HOST_STOP_TRACE");
    const bool trace_enabled = trace_value != nullptr && std::string_view (trace_value) != "0"
                               && std::string_view (trace_value) != "";
    if (trace_enabled) {
        std::cerr << "zlink-cpp-host-stop stage=before-app-state-destroy-services" << std::endl;
    }
    hosted_services.clear ();
    {
        std::unique_lock lock (status_access->mutex);
        status_access->state = nullptr;
    }
    host_capacity.reset ();
    monitoring->runtime_failures->capture ([&] {
        // App owns the Core context. Retained service references must not
        // keep its transport running after the App scope has ended.
        if (auto context = zlink_builder_access_t::shared_core_context (zlink))
            context->term ();
    });
    if (trace_enabled) {
        std::cerr << "zlink-cpp-host-stop stage=after-app-state-destroy-services" << std::endl;
    }
}

void app_state_t::start_hosted_services (service_provider_t &provider,
                                         std::vector<hosted_service_t *> &started)
{
    for (const auto &service : hosted_services) {
        service->start (provider).result ().value ();
        started.push_back (service.get ());
    }
}

// Location runtime §5 allows startup to finish before owner-bound publication.
task_t<void>
app_state_t::enter_serving_after_publication (std::shared_ptr<app_state_access_t> access,
                                              task_t<bool> published)
{
    const auto result = co_await runtime::await_result (std::move (published));
    if (!result) {
        std::shared_lock lock (access->mutex);
        if (auto *state = access->state)
            state->runtime_state.store (framework_runtime_state_t::error,
                                        std::memory_order_release);
        (void) result.value ();
    }
    if (result.value ()) {
        std::shared_lock lock (access->mutex);
        if (auto *state = access->state) {
            auto expected = framework_runtime_state_t::preparing;
            (void) state->runtime_state.compare_exchange_strong (
              expected, framework_runtime_state_t::serving, std::memory_order_acq_rel);
        }
    }
}

void app_state_t::close_framework_resources (service_provider_t &provider) noexcept
{
    if (framework_options)
        dispatch_options_access_t::retire_telemetry (framework_options->dispatch_options ());
    const auto &failures = monitoring->runtime_failures;
    failures->capture ([&] { channel_runtime_t::from (zlink.message_bus ()).shutdown (); });
    failures->capture ([&] { drain_zlink_builder_runtime (zlink); });
    failures->capture ([&] { runtime::shutdown_handler_coroutine_executor (); });
    failures->capture ([&] { shutdown_stream_dispatch_executor (); });
    failures->capture ([&] { shutdown_handler_invocation_executor (); });
    failures->capture ([&] { provider.close (); });
}

void app_state_t::stop_hosted_services (const std::vector<hosted_service_t *> &started) noexcept
{
    monitoring->runtime_failures->bind_runtime_reporter ({});
    monitoring->runtime_failures->close_retained ();
    const char *trace_value = std::getenv ("ZLINK_CPP_HOST_STOP_TRACE");
    const bool trace_enabled = trace_value != nullptr && std::string_view (trace_value) != "0"
                               && std::string_view (trace_value) != "";
    auto stop_service = [trace_enabled] (hosted_service_t *service) {
        if (trace_enabled) {
            std::cerr << "zlink-cpp-host-stop stage=before service=" << typeid (*service).name ()
                      << std::endl;
        }
        service->stop ();
        if (trace_enabled) {
            std::cerr << "zlink-cpp-host-stop stage=after service=" << typeid (*service).name ()
                      << std::endl;
        }
    };
    std::vector<hosted_service_t *> request_order (started.rbegin (), started.rend ());
    std::stable_sort (request_order.begin (), request_order.end (), [] (auto *left, auto *right) {
        const auto *left_lifecycle = lifecycle_of (left);
        const auto *right_lifecycle = lifecycle_of (right);
        const auto left_priority =
          left_lifecycle ? left_lifecycle->shutdown_request_priority () : 0;
        const auto right_priority =
          right_lifecycle ? right_lifecycle->shutdown_request_priority () : 0;
        return left_priority > right_priority;
    });
    for (auto *service : request_order)
        service->request_stop ();

    std::vector<hosted_service_t *> stop_order (started.rbegin (), started.rend ());
    std::stable_sort (stop_order.begin (), stop_order.end (), [] (auto *left, auto *right) {
        const auto *left_lifecycle = lifecycle_of (left);
        const auto *right_lifecycle = lifecycle_of (right);
        const auto left_priority = left_lifecycle ? left_lifecycle->shutdown_stop_priority () : 0;
        const auto right_priority =
          right_lifecycle ? right_lifecycle->shutdown_stop_priority () : 0;
        return left_priority > right_priority;
    });
    for (auto *service : stop_order)
        stop_service (service);
}

} // namespace zlink::framework::detail

namespace
{

volatile std::sig_atomic_t g_stop_signal_requested = 0;

void handle_process_signal (int) noexcept
{
    g_stop_signal_requested = 1;
}

} // namespace

namespace zlink::framework
{

app_t::app_t () : _state (std::make_unique<detail::app_state_t> ())
{
    _state->services.add_factory<detail::delay_owner_t> (
      [owner = &_state->delays] (service_provider_t &) {
          return std::shared_ptr<detail::delay_owner_t> (owner, [] (auto *) noexcept {});
      },
      service_lifetime_t::singleton);
}

task_t<bool> detail::app_test_access_t::startup_publication (app_t &app)
{
    return app._state->startup_publication ();
}

const std::atomic_bool &detail::app_test_access_t::stop_requested (app_t &app)
{
    return app._state->stop_requested;
}

std::uint64_t detail::app_test_access_t::admission_queue_epoch (const app_t &app)
{
    for (const auto &service : app._state->hosted_services) {
        if (const auto *streams =
              dynamic_cast<const runtime::stream_host_service_t *> (service.get ())) {
            if (const auto queue = streams->_ingress->queue ()) {
                const std::lock_guard lock (queue->_mutex);
                return queue->_epoch;
            }
        }
    }
    return 0;
}

std::optional<std::string> detail::app_test_access_t::stream_endpoint (app_t &app,
                                                                       const std::string &name)
{
    const auto status = app._state->listener_statuses->find (listener_kind_t::stream, name);
    if (status && !status->endpoint.empty ())
        return status->endpoint;
    return {};
}

app_t::~app_t ()
{
    if (_state) {
        _state->delays.stop_and_join ();
        _state->wait_relocation_cleanup ();
    }
}

app_t::app_t (app_t &&) noexcept = default;

app_t &app_t::operator= (app_t &&) noexcept = default;

app_t app_t::create ()
{
    return {};
}

app_advanced_t::app_advanced_t (app_t &app) noexcept : _app (&app)
{
}

service_collection_t &app_advanced_t::services () noexcept
{
    return _app->_services ();
}

handler_registry_t &app_advanced_t::handlers () noexcept
{
    return _app->_handlers ();
}

zlink_builder_t &app_advanced_t::zlink () noexcept
{
    return _app->_zlink_builder ();
}

config_builder_t &app_t::config () noexcept
{
    return _state->config;
}

logging_builder_t &app_t::logging () noexcept
{
    return _state->logging;
}

monitoring_builder_t &app_t::monitoring () noexcept
{
    return _state->monitoring_builder;
}

app_t &app_t::set_message_flow_mode (message_flow_log_mode_t mode) noexcept
{
    auto operation = set_message_flow_mode_async (mode);
    (void) detail::task_result_access_t::read (operation);
    return *this;
}

task_t<void> app_t::set_message_flow_mode_async (message_flow_log_mode_t mode) noexcept
{
    // Note: before apply() this is overwritten by the configured mode (config seeds
    // at apply); the intended use is runtime toggling after the app is running.
    _state->message_flow_mode->store (mode, std::memory_order_relaxed);
    return task_t<void> (result_t<void>::success ());
}

message_flow_log_mode_t app_t::message_flow_mode () const noexcept
{
    return _state->message_flow_mode->load (std::memory_order_relaxed);
}

health_builder_t &app_t::health () noexcept
{
    return _state->health;
}

app_advanced_t app_t::advanced () noexcept
{
    return app_advanced_t (*this);
}

service_collection_t &app_t::_services () noexcept
{
    return _state->services;
}

handler_registry_t &app_t::_handlers () noexcept
{
    return _state->handlers;
}

zlink_builder_t &app_t::_zlink_builder () noexcept
{
    return _state->zlink;
}

serializer_registry_t &app_t::_serializers () noexcept
{
    return _state->serializers;
}

app_t &app_t::add_module (module_t &module)
{
    module.configure_services (_state->services);
    module.configure_zlink (_state->zlink);
    module.configure_handlers (_state->handlers);
    return *this;
}

app_t &app_t::add_hosted_service (std::unique_ptr<hosted_service_t> service)
{
    if (!service) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "hosted service must not be null");
    }
    if (auto *lifecycle = detail::lifecycle_of (service.get ()))
        lifecycle->bind_runtime_failures (_state->monitoring->runtime_failures);
    _state->hosted_services.push_back (std::move (service));
    return *this;
}

app_t &app_t::use_metric_provider (runtime::metric_provider_t provider)
{
    std::lock_guard lock (_state->termination_teardown_mutex);
    if (_state->run_started->load (std::memory_order_acquire))
        throw std::logic_error ("Metric provider must be configured before run");
    _state->monitoring->use_metric_provider (std::move (provider));
    _state->runtime_state.connect (_state->monitoring->metric_provider);
    return *this;
}

int app_t::run (int argc, char **argv)
try {
    {
        std::lock_guard lock (_state->termination_teardown_mutex);
        _state->run_started->store (true, std::memory_order_release);
    }
    const completion_owner_t completion_owner;
    const auto completion_dispatcher = detail::borrow_completion_dispatcher ();
    std::optional<service_provider_t> provider;
    std::vector<hosted_service_t *> started;
    task_t<void> startup_publication (result_t<void>::success ());
    std::exception_ptr startup_failure;
    try {
        _apply_zlink_framework ();
        _state->config.load_cli (argc, argv);
        detail::serializer_registry_access_t::freeze (_state->serializers);
        _state->handlers.bind_codecs (_state->serializers);
        _state->config.model ().set ("host.signal_handlers", "installed");
        g_stop_signal_requested = 0;
        std::signal (SIGINT, handle_process_signal);
        std::signal (SIGTERM, handle_process_signal);
#ifdef _WIN32
        std::signal (SIGBREAK, handle_process_signal);
#endif

        provider.emplace (_state->services.build_provider ());
        {
            std::lock_guard lock (_state->termination_teardown_mutex);
            if (_state->runtime_state.load (std::memory_order_acquire)
                == framework_runtime_state_t::stopped)
                return 0;
            _state->run_active = true;
            _state->teardown_complete = false;
        }
        runtime::install_host_context_hooks ();
        runtime::configure_handler_coroutine_executor (
          _state->framework_options ? _state->framework_options->handler_coroutine_workers () : 0,
          completion_dispatcher);
        _state->start_hosted_services (*provider, started);
        startup_publication = detail::app_state_t::enter_serving_after_publication (
          _state->status_access, _state->startup_publication ());
        while (!_state->stop_requested.load (std::memory_order_acquire)) {
            if (startup_publication.await_ready () && !startup_publication.result ()) {
                std::unique_lock lock (_state->termination_operation.mutex);
                if (!_state->termination_operation.started) {
                    lock.unlock ();
                    (void) shutdown ();
                }
            }
            _state->delays.reap_completed ();
            if (g_stop_signal_requested != 0) {
                g_stop_signal_requested = 0;
                (void) shutdown ();
            }
            zlink::framework::runtime::wait_poll_interval (std::chrono::milliseconds (1));
        }
    }
    catch (...) {
        startup_failure = std::current_exception ();
        _state->runtime_state.store (framework_runtime_state_t::error, std::memory_order_release);
    }

    // Reserve the shared termination operation before releasing any resources.
    // A concurrent shutdown either joins this teardown or already owns its worker.
    {
        std::lock_guard lock (_state->termination_operation.mutex);
        _state->termination_operation.started = true;
    }
    if (_state->termination_operation.worker.joinable ()) {
        std::unique_lock lock (_state->termination_teardown_mutex);
        _state->termination_teardown_changed.wait (
          lock, [&] { return _state->stop_requested.load (std::memory_order_acquire); });
    }
    _state->delays.stop_and_join ();
    _state->wait_relocation_cleanup ();
    _state->stop_hosted_services (started);
    if (provider)
        _state->close_framework_resources (*provider);
    {
        std::lock_guard lock (_state->termination_teardown_mutex);
        _state->run_active = false;
        _state->teardown_complete = true;
    }
    _state->termination_teardown_changed.notify_all ();
    if (_state->termination_operation.worker.joinable ())
        _state->termination_operation.worker.join ();
    else
        _state->complete_termination ({termination_outcome_t::stopped, termination_reason_t::none});
    if (startup_failure)
        std::rethrow_exception (startup_failure);
    if (startup_publication.await_ready ())
        startup_publication.result ().value ();
    _state->runtime_state.store (framework_runtime_state_t::stopped, std::memory_order_release);
    _state->monitoring->runtime_failures->rethrow_if_failed ();
    return 0;
}
catch (const std::exception &error) {
    _state->runtime_state.store (framework_runtime_state_t::error, std::memory_order_release);
    const auto message = std::string ("zlink application failed: ") + error.what ();
    std::cerr << message << '\n';
    try {
        _state->logging.create_logger ("zlink.framework.host").critical (message);
    }
    catch (const std::exception &logging_error) {
        std::cerr << "zlink application failure logging failed: " << logging_error.what () << '\n';
    }
    return 1;
}
catch (...) {
    _state->runtime_state.store (framework_runtime_state_t::error, std::memory_order_release);
    constexpr std::string_view message = "zlink application failed with an unknown exception";
    std::cerr << message << '\n';
    try {
        _state->logging.create_logger ("zlink.framework.host").critical (std::string (message));
    }
    catch (const std::exception &logging_error) {
        std::cerr << "zlink application failure logging failed: " << logging_error.what () << '\n';
    }
    return 1;
}

bool app_t::is_ready () const noexcept
{
    return runtime_state () == framework_runtime_state_t::serving;
}

framework_runtime_state_t app_t::runtime_state () const noexcept
{
    return _state->runtime_state.load (std::memory_order_acquire);
}

void app_t::stop () noexcept
{
    try {
        (void) shutdown ();
    }
    catch (...) {
        _state->delays.request_stop ();
        _state->stop_requested.store (true, std::memory_order_release);
    }
}

void app_t::request_stop () noexcept
{
    stop ();
}

} // namespace zlink::framework
