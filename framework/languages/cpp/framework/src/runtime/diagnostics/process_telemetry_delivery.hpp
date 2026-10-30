/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <zlink/framework/contracts/dispatch/execution.hpp>
#include "runtime/diagnostics/diagnostic_event_sink.hpp"
#include "runtime/diagnostics/dispatch_events.hpp"
#include "runtime/diagnostics/metric_provider.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <variant>

namespace zlink::framework::detail
{
enum class telemetry_event_kind_t
{
    message_flow,
    dispatch_error,
    any
};
bool process_telemetry_has_provider (const dispatch_options_t &options,
                                     telemetry_event_kind_t kind) noexcept;

struct telemetry_record_t
{
    std::uint64_t registration = 0;
    std::variant<message_flow_event_t, message_dispatch_error_event_t> event;
    message_flow_log_mode_t mode = message_flow_log_mode_t::off;
    bool include_sizes = false;
    std::optional<diagnostic_event_sink_t::exception_summary_t> error;
    std::optional<logger_t<>> logger;
    std::function<void (const void *)> observer;
};

// Registration and delivery belong to one process owner; records never retain a host.
class telemetry_registration_t
{
  public:
    explicit telemetry_registration_t (std::uint64_t value) : id (value) {}
    ~telemetry_registration_t ();
    const std::uint64_t id;
};
std::shared_ptr<telemetry_registration_t> register_process_telemetry ();
void bind_process_telemetry_metrics (
  std::uint64_t registration,
  runtime::metric_provider_t provider,
  opentelemetry::nostd::shared_ptr<opentelemetry::metrics::Meter> meter);
void retire_process_telemetry (std::uint64_t registration) noexcept;
void submit_process_telemetry (telemetry_record_t record) noexcept;
std::atomic<std::uint64_t> &telemetry_traced_count () noexcept;
std::atomic<std::uint64_t> &telemetry_reported_count () noexcept;
std::atomic<std::uint64_t> &telemetry_failure_count () noexcept;
std::uint64_t telemetry_dropped () noexcept;
} // namespace zlink::framework::detail
