/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/dispatch/execution.hpp>

#include "runtime/diagnostics/diagnostic_event_sink.hpp"
#include "runtime/diagnostics/dispatch_diagnostics_names.hpp"
#include "runtime/diagnostics/flow_context.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace zlink::framework::detail
{

// Emits success-path message-flow transitions, gated by the diagnostics mode
// (live/runtime-mutable). It is the success-path twin of dispatch_error_reporter_t,
// sharing the logger + offloaded-observer fan-out so errors and healthy transitions
// read as one correlation-id-keyed stream.
//
// PERFORMANCE: the tracer only borrows dispatch_options (no copy), and the mode
// gate is a single relaxed atomic load + compare. Callers MUST build the event
// behind `enabled(...)` (or use the lazy trace overload) so that when tracing is
// off there is ZERO allocation on the dispatch hot path.
//
// Mode gating (modes are ordered off < errors_only < key_transitions < verbose
// < diagnostic):
//   * received / dispatched / replied / sent / reply_received require key_transitions+.
//   * dropped / error require errors_only or higher.
//   * message sizes are appended only at verbose+ when include_message_sizes.
class message_flow_tracer_t
{
  public:
    explicit message_flow_tracer_t (const dispatch_options_t &options) : _options (&options) {}

    bool enabled (message_flow_log_mode_t min_mode) const noexcept
    {
        return rank (_options->diagnostics.effective_message_flow ()) >= rank (min_mode);
    }

    static message_flow_log_mode_t required_mode (message_flow_outcome_t outcome) noexcept
    {
        return (outcome == message_flow_outcome_t::dropped
                || outcome == message_flow_outcome_t::error)
                 ? message_flow_log_mode_t::errors_only
                 : message_flow_log_mode_t::key_transitions;
    }

    bool enabled_for (message_flow_outcome_t outcome) const noexcept
    {
        return enabled (required_mode (outcome));
    }

    /* flow-correlation §2.2: host entry points create new flow ids only when
     * tracing is not fully off; propagation of inbound ids is unconditional. */
    bool capture_enabled () const noexcept
    {
        return enabled (message_flow_log_mode_t::errors_only);
    }

    // Lazy form: the event (and its string fields) is built only after the cheap
    // mode/sample gate passes, so an "off" dispatch pays nothing but the gate.
    template <typename Fn>
    void trace (message_flow_outcome_t outcome, Fn &&build_event) const noexcept
    {
        if (!enabled_for (outcome)) {
            return;
        }
        // build_event() builds the event (allocates strings); guard it so a throw
        // (e.g. bad_alloc) never terminates this noexcept tracing call.
        try {
            auto event = build_event ();
            stamp_flow (event);
            if (outcome != message_flow_outcome_t::dropped
                && outcome != message_flow_outcome_t::error
                && !sample (_options->diagnostics.sample_rate (), event.flow_id)) {
                return;
            }
            emit (std::move (event));
        }
        catch (...) {
            observer_failure_count ().fetch_add (1, std::memory_order_relaxed);
        }
    }

    // Eager form: prefer the lazy overload on hot paths. This still gates before
    // doing any work, but the caller has already paid to build the event.
    void trace (message_flow_event_t event) const noexcept
    {
        if (!enabled_for (event.outcome)) {
            return;
        }
        stamp_flow (event);
        if (event.outcome != message_flow_outcome_t::dropped
            && event.outcome != message_flow_outcome_t::error
            && !sample (_options->diagnostics.sample_rate (), event.flow_id)) {
            return;
        }
        emit (std::move (event));
    }

  private:
    void emit (message_flow_event_t event) const noexcept
    {
        traced_count ().fetch_add (1, std::memory_order_relaxed);
        log_default (event);

        diagnostic_event_sink_t::deliver_observer (
          _options->message_flow_observer, _options->message_flow_callback, std::move (event),
          [] (message_flow_observer_t &observer, const message_flow_event_t &event) {
              observer.on_message_flow (event);
          },
          [] (const std::function<void (const message_flow_event_t &)> &callback,
              const message_flow_event_t &event) { callback (event); },
          [logger = _options->diagnostics_logger] {
              observer_failure_count ().fetch_add (1, std::memory_order_relaxed);
              diagnostic_event_sink_t::log_or_clog (logger, log_level_t::error,
                                                    "message flow observer failed",
                                                    "zlink framework observer error:", {});
          },
          [] { observer_dropped_count ().fetch_add (1, std::memory_order_relaxed); });
    }

  public:
    static std::uint64_t traced () noexcept
    {
        return traced_count ().load (std::memory_order_relaxed);
    }

    static std::uint64_t observer_failures () noexcept
    {
        return observer_failure_count ().load (std::memory_order_relaxed);
    }

    static std::uint64_t observer_dropped () noexcept
    {
        return observer_dropped_count ().load (std::memory_order_relaxed);
    }

  private:
    static int rank (message_flow_log_mode_t mode) noexcept { return static_cast<int> (mode); }

    static const char *flow_origin_name (flow_origin_t origin) noexcept
    {
        switch (origin) {
            case flow_origin_t::inbound:
                return "inbound";
            case flow_origin_t::timer:
                return "timer";
            case flow_origin_t::application:
                return "application";
            case flow_origin_t::lifecycle:
                return "lifecycle";
        }
        return "unknown";
    }

    static const char *terminal_outcome_name (
      message_flow_outcome_t outcome) noexcept
    {
        switch (outcome) {
            case message_flow_outcome_t::error:
                return "failed";
            case message_flow_outcome_t::dropped:
                return "dropped";
            default:
                return "succeeded";
        }
    }

    /* Fills the optional flow pair from the ambient context when the caller
     * did not carry it (flow-correlation §8: both or neither). */
    static void stamp_flow (message_flow_event_t &event)
    {
        if (event.flow_id.has_value () != event.flow_origin.has_value ()) {
            event.flow_id.reset ();
            event.flow_origin.reset ();
        }
        if (!event.flow_id) {
            if (const auto &flow = runtime::flow_context_t::current ()) {
                event.flow_id = flow->flow_id;
                event.flow_origin = flow->origin;
            }
        }
    }

    /* Flow-consistent sampling (flow-correlation §5): thinning is keyed by
     * the flow id hash so one flow is kept or dropped as a whole; events
     * without a flow fall back to stride sampling. */
    bool sample (double rate, const std::optional<std::string> &flow_id) const noexcept
    {
        if (rate >= 1.0) {
            return true;
        }
        if (rate <= 0.0) {
            return false;
        }
        if (flow_id) {
            const auto hash = std::hash<std::string>{} (*flow_id);
            return (static_cast<double> (hash % 10000) / 10000.0) < rate;
        }
        auto stride = static_cast<std::uint64_t> (1.0 / rate + 0.5);
        if (stride == 0) {
            stride = 1;
        }
        return (sample_counter ().fetch_add (1, std::memory_order_relaxed) % stride) == 0;
    }

    void log_default (const message_flow_event_t &event) const noexcept
    {
        try {
            // Build structured key/value fields once (so collectors can ingest
            // without parsing text); reuse them for the clog fallback too.
            std::vector<log_field_t> fields;
            fields.reserve (14);
            auto add = [&fields] (const char *key, std::string value) {
                diagnostic_event_sink_t::append_field (fields, key, std::move (value));
            };
            add ("event_id", "zlink.message_flow");
            add ("phase", std::string (enum_name (event.outcome)));
            add ("outcome", terminal_outcome_name (event.outcome));
            add ("surface", std::string (enum_name (event.surface)));
            add ("kind", std::string (enum_name (event.message_kind)));
            if (_options->diagnostics.label ()) {
                add ("label", *_options->diagnostics.label ());
            }
            if (event.packet_name) {
                add ("packet", *event.packet_name);
            }
            if (event.channel_name) {
                add ("channel", *event.channel_name);
            }
            if (event.topic) {
                add ("topic", *event.topic);
            }
            if (event.correlation_id) {
                add ("corr", *event.correlation_id);
            }
            if (event.flow_id) {
                add ("flow", *event.flow_id);
            }
            if (event.flow_origin) {
                add ("origin", std::string (flow_origin_name (*event.flow_origin)));
            }
            if (event.source_rid) {
                add ("src", *event.source_rid);
            }
            if (event.spot_id) {
                add ("spot", *event.spot_id);
            }
            if (event.actor_id) {
                add ("actor", *event.actor_id);
            }
            if (event.error_reason) {
                add ("reason", std::string (enum_name (*event.error_reason)));
            }
            if (event.error_action) {
                add ("action", std::string (enum_name (*event.error_action)));
            }
            if (event.message_size && enabled (message_flow_log_mode_t::verbose)
                && _options->diagnostics.include_message_sizes ()) {
                add ("size", std::to_string (*event.message_size));
            }
            // Prefer the framework logger with structured fields (so a file/console
            // sink renders them and a collector callback gets record.fields); fall
            // back to a flat clog line when no logger is wired (tests, no-app usage).
            diagnostic_event_sink_t::log_or_clog (_options->diagnostics_logger, log_level_t::info,
                                                  "message flow",
                                                  "zlink flow:", std::move (fields));
        }
        catch (...) {
            observer_failure_count ().fetch_add (1, std::memory_order_relaxed);
        }
    }

    static std::atomic<std::uint64_t> &sample_counter () noexcept
    {
        static std::atomic<std::uint64_t> value{0};
        return value;
    }

    static std::atomic<std::uint64_t> &traced_count () noexcept
    {
        static std::atomic<std::uint64_t> value{0};
        return value;
    }

    static std::atomic<std::uint64_t> &observer_failure_count () noexcept
    {
        static std::atomic<std::uint64_t> value{0};
        return value;
    }

    static std::atomic<std::uint64_t> &observer_dropped_count () noexcept
    {
        static std::atomic<std::uint64_t> value{0};
        return value;
    }

    const dispatch_options_t *_options;
};

} // namespace zlink::framework::detail
