/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/configuration/logging.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace zlink::framework
{

namespace detail
{
class dispatch_options_access_t;
}

enum class handler_execution_t
{
    inline_on_runtime = 0,
    offload = 1
};

enum class message_flow_log_mode_t
{
    off = 0,
    errors = 1,
    normal = 2,
    detailed = 3
};

enum class flow_origin_t : std::uint8_t
{
    inbound = 1,
    timer = 2,
    application = 3,
    lifecycle = 4
};

// Dispatch diagnostics configuration. Application logging and telemetry providers
// own output routing; this value only controls what the runtime records.
class dispatch_diagnostics_options_t
{
  public:
    message_flow_log_mode_t message_flow () const noexcept { return _message_flow; }
    double sample_rate () const noexcept { return _sample_rate; }
    bool include_message_sizes () const noexcept { return _include_message_sizes; }

  private:
    // Only the dispatch options builder writes these (enforces builder-only config).
    friend struct dispatch_options_t;

    message_flow_log_mode_t _message_flow = message_flow_log_mode_t::errors;
    double _sample_rate = 1.0;
    bool _include_message_sizes = true;
};

struct dispatch_options_t
{
    dispatch_diagnostics_options_t diagnostics;
    // When set, message-flow transitions and dispatch errors are emitted through
    // the framework logger (so app.logging().use_file(...) captures them) instead
    // of the std::clog fallback. Wired by the host at apply() only if a logging
    // output sink is configured; otherwise left empty to preserve clog behavior.
    // Fluent diagnostics/tracing config (chains off configure_dispatch()). This is
    // the only supported way to set diagnostics (the fields are encapsulated).
    dispatch_options_t &message_flow (message_flow_log_mode_t mode)
    {
        diagnostics._message_flow = mode;
        return *this;
    }

    dispatch_options_t &trace_sample_rate (double rate)
    {
        diagnostics._sample_rate = rate;
        return *this;
    }

    dispatch_options_t &include_message_sizes (bool include)
    {
        diagnostics._include_message_sizes = include;
        return *this;
    }

  private:
    friend class detail::dispatch_options_access_t;

    std::optional<logger_t<>> _diagnostics_logger;
    std::function<void (const void *)> _message_flow_observer;
    std::shared_ptr<std::atomic<message_flow_log_mode_t>> _live_mode;
};

} // namespace zlink::framework
