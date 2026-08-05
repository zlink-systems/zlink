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

enum class handler_execution_t
{
    inline_on_runtime = 0,
    offload = 1
};

enum class message_flow_log_mode_t
{
    off = 0,
    errors_only = 1,
    key_transitions = 2,
    verbose = 3,
    diagnostic = 4
};

// A transition or error result in a message's lifecycle. It lets healthy traffic
// and dispatch failures share one observer stream keyed by correlation id.
enum class message_flow_outcome_t
{
    received = 0,       // a well-formed envelope arrived at a dispatch surface (inbound)
    dispatched = 1,     // a fire-and-forget message was handed to its handler (inbound)
    replied = 2,        // a request completed and a reply was produced (inbound)
    dropped = 3,        // a message was intentionally discarded (no handler, decode failed, ...)
    sent = 4,           // a message left this node toward another channel/spot/node (outbound)
    reply_received = 5, // a reply came back for an outbound request (outbound)
    error = 6           // dispatch failed before the normal lifecycle transition completed
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
    // else the configured mode. Read live on every dispatch (relaxed atomic load).
    message_flow_log_mode_t effective_message_flow () const noexcept
    {
        return _live_mode ? _live_mode->load (std::memory_order_relaxed) : _message_flow;
    }

  private:
    // Only the dispatch options builder writes these (enforces builder-only config).
    friend struct dispatch_options_t;

    message_flow_log_mode_t _message_flow = message_flow_log_mode_t::errors_only;
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

enum class dispatch_error_surface_t
{
    channel = 0,
    route_mesh_channel = 1,
    spot_route = 2,
    spot_subscription = 3,
    spot_actor = 4,
    stream_session = 5
};

enum class dispatch_message_kind_t
{
    request = 0,
    send = 1,
    publish = 2,
    response = 3,
    error = 4,
    actor_request = 5,
    actor_send = 6
};

enum class dispatch_error_reason_t
{
    handler_missing = 0,
    payload_decode_failed = 1,
    handler_exception = 2,
    invalid_frame = 3,
    reply_path_missing = 4,
    unexpected_reply = 5
};

/* framework API §2.4.3: reply frame이 있는 request는 error reply로 끝나고(reply_error),
 * one-way는 drop한다. reply frame이 없는 경로(같은 process 안의 local actor 호출 등)는
 * caller의 task를 framework 오류로 완료하며 그 실패를 fail_caller로 관측한다. */
enum class dispatch_error_action_t
{
    reply_error = 0,
    drop = 1,
    fail_caller = 2
};

/* Root origin of a message flow (flow-correlation §4.2). Wire values are
 * fixed: 1=inbound, 2=timer, 3=application, 4=lifecycle. */
enum class flow_origin_t : std::uint8_t
{
    inbound = 1,
    timer = 2,
    application = 3,
    lifecycle = 4
};

struct message_dispatch_error_event_t
{
    dispatch_error_surface_t surface;
    dispatch_message_kind_t message_kind;
    dispatch_error_reason_t reason;
    dispatch_error_action_t action;
    std::optional<std::string> packet_name;
    std::optional<std::string> channel_name;
    std::optional<std::string> topic;
    std::optional<std::string> spot_id;
    std::optional<std::string> actor_id;
    std::optional<std::string> source_rid;
    std::optional<std::string> correlation_id;
    std::exception_ptr exception;
    /* Optional pair: both present or both absent (flow-correlation §8). */
    std::optional<std::string> flow_id;
    std::optional<flow_origin_t> flow_origin;
};

struct message_flow_event_t
{
    message_flow_outcome_t outcome;
    dispatch_error_surface_t surface;
    dispatch_message_kind_t message_kind;
    std::optional<std::string> packet_name;
    std::optional<std::string> channel_name;
    std::optional<std::string> topic;
    std::optional<std::string> correlation_id;
    std::optional<std::string> source_rid;
    std::optional<std::string> spot_id;
    std::optional<std::string> actor_id;
    std::optional<std::size_t> message_size;
    std::optional<dispatch_error_reason_t> error_reason;
    std::optional<dispatch_error_action_t> error_action;
    std::exception_ptr exception;
    /* Optional pair: both present or both absent (flow-correlation §8). */
    std::optional<std::string> flow_id;
    std::optional<flow_origin_t> flow_origin;
};

class message_flow_observer_t
{
  public:
    virtual ~message_flow_observer_t () = default;
    virtual void on_message_flow (const message_flow_event_t &event) = 0;
};

struct dispatch_options_t
{
    dispatch_diagnostics_options_t diagnostics;
    std::shared_ptr<message_flow_observer_t> message_flow_observer;
    std::function<void (const message_flow_event_t &)> message_flow_callback;
    // When set, message-flow transitions and dispatch errors are emitted through
    // the framework logger (so app.logging().use_file(...) captures them) instead
    // of the std::clog fallback. Wired by the host at apply() only if a logging
    // output sink is configured; otherwise left empty to preserve clog behavior.
    std::optional<logger_t<>> diagnostics_logger;

    dispatch_options_t &
    set_message_flow_observer (std::shared_ptr<message_flow_observer_t> observer)
    {
        message_flow_observer = std::move (observer);
        message_flow_callback = {};
        return *this;
    }

    dispatch_options_t &
    set_message_flow_observer (std::function<void (const message_flow_event_t &)> observer)
    {
        message_flow_callback = std::move (observer);
        message_flow_observer.reset ();
        return *this;
    }

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
};

} // namespace zlink::framework
