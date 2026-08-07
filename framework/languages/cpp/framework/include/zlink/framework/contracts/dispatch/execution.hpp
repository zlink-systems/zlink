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

// Diagnostics/tracing config. Fields are encapsulated: configure them only through
// the fluent builder on dispatch_options_t (configure_dispatch().message_flow(...)
// .trace_log_file(...).trace_label(...)...). Read access is via getters.
class dispatch_diagnostics_options_t
{
  public:
    // Configured (static) mode. Prefer effective_message_flow() for runtime checks.
    message_flow_log_mode_t message_flow () const noexcept { return _message_flow; }
    double sample_rate () const noexcept { return _sample_rate; }
    bool include_message_sizes () const noexcept { return _include_message_sizes; }
    const std::optional<std::string> &log_file () const noexcept { return _log_file; }
    const std::optional<std::string> &label () const noexcept { return _label; }
    const std::shared_ptr<std::atomic<message_flow_log_mode_t>> &live_mode () const noexcept
    {
        return _live_mode;
    }

    // The mode actually in effect: the live (runtime-mutable) override if installed,
    // else the configured mode. Runtime entry points snapshot this value once so
    // a change applies to the next message rather than halfway through one message.
    message_flow_log_mode_t effective_message_flow () const noexcept
    {
        return _live_mode ? _live_mode->load (std::memory_order_relaxed) : _message_flow;
    }

  private:
    // Only the dispatch options builder writes these (enforces builder-only config).
    friend struct dispatch_options_t;

    message_flow_log_mode_t _message_flow = message_flow_log_mode_t::errors;
    double _sample_rate = 1.0;
    bool _include_message_sizes = true;
    // When set, tracing/error logs go to this dedicated file (separated from app
    // logs). Empty = shared app logger (or std::clog if no sink).
    std::optional<std::string> _log_file;
    // Runtime label stamped on each trace line (`label=`) for cross-node
    // aggregation of process-local correlation ids. App-provided; empty = omitted.
    std::optional<std::string> _label;
    // Shared, runtime-mutable mode override (installed by the host at apply); copying
    // dispatch options shares the same atomic so every surface observes changes.
    std::shared_ptr<std::atomic<message_flow_log_mode_t>> _live_mode;
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

    // Send message-flow/error tracing to its own file (separated from app logs).
    dispatch_options_t &trace_log_file (std::string path)
    {
        diagnostics._log_file = std::move (path);
        return *this;
    }

    // Runtime label stamped on every trace line (`label=`) for cross-node
    // aggregation of process-local correlation ids.
    dispatch_options_t &trace_label (std::string id)
    {
        diagnostics._label = std::move (id);
        return *this;
    }

    // Install a shared, runtime-mutable mode cell (host wiring for runtime toggle;
    // app_t::set_message_flow_mode flips it). Overrides the configured message_flow.
    dispatch_options_t &
    message_flow_live (std::shared_ptr<std::atomic<message_flow_log_mode_t>> live)
    {
        diagnostics._live_mode = std::move (live);
        return *this;
    }

  private:
    friend class detail::dispatch_options_access_t;

    std::optional<logger_t<>> _diagnostics_logger;
    std::function<void (const void *)> _message_flow_observer;
};

} // namespace zlink::framework
