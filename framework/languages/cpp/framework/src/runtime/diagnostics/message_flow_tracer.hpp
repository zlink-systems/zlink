/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/dispatch/execution.hpp>

#include "runtime/diagnostics/diagnostic_event_sink.hpp"
#include "runtime/diagnostics/dispatch_diagnostics_names.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/diagnostics/flow_context.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace zlink::framework::detail
{

// Emits message-flow transitions, gated by the live process-wide diagnostics mode
// at each processing point. It is the success-path
// twin of dispatch_error_reporter_t,
// sharing the logger + offloaded-observer fan-out so errors and healthy transitions
// read as one correlation-id-keyed stream.
//
// PERFORMANCE: the tracer only borrows dispatch_options (no copy), and the mode
// gate is a single relaxed atomic load + compare. Callers MUST build the event
// behind `enabled(...)` (or use the lazy trace overload) so that when tracing is
// off there is ZERO allocation on the dispatch hot path.
//
// Mode gating (modes are ordered off < errors < normal < detailed):
//   * received / admitted / dispatched / replied / sent / reply_received require normal+.
//   * dropped / backpressured require errors or higher.
//   * Callers that use detail_stage for internal pipeline tracing must also
//     gate the call with enabled(detailed); detail fields alone do not raise
//     the event's required mode.
//   * message sizes are appended only at detailed when include_message_sizes.
class message_flow_tracer_t
{
  public:
    explicit message_flow_tracer_t (const dispatch_options_t &options) :
        _options (&options)
    {
    }

    bool enabled (message_flow_log_mode_t min_mode) const noexcept
    {
        const auto mode = dispatch_options_access_t::effective_message_flow (*_options);
        return rank (mode) >= rank (min_mode);
    }

    message_flow_log_mode_t mode () const noexcept
    {
        return dispatch_options_access_t::effective_message_flow (*_options);
    }

    static message_flow_log_mode_t required_mode (message_flow_outcome_t outcome) noexcept
    {
        return (outcome == message_flow_outcome_t::dropped
                || outcome == message_flow_outcome_t::backpressured)
                 ? message_flow_log_mode_t::errors
                 : message_flow_log_mode_t::normal;
    }

    bool enabled_for (message_flow_outcome_t outcome) const noexcept
    {
        return enabled (required_mode (outcome));
    }

    /* flow-correlation §2.2: host entry points create new flow ids only when
     * tracing is not fully off. */
    bool capture_enabled () const noexcept
    {
        return enabled (message_flow_log_mode_t::errors);
    }

    // Lazy form: the event (and its string fields) is built only after the cheap
    // mode/sample gate passes, so an "off" dispatch pays nothing but the gate.
    template <typename Fn>
    void trace (message_flow_outcome_t outcome, Fn &&build_event) const noexcept
    {
        trace (required_mode (outcome), outcome, std::forward<Fn> (build_event));
    }

    template <typename Fn>
    void trace (message_flow_outcome_t outcome,
                std::string_view sampling_flow_id,
                Fn &&build_event) const noexcept
    {
        trace_with_sampling_key (required_mode (outcome), outcome,
                                 sampling_flow_id,
                                 std::forward<Fn> (build_event));
    }

    template <typename Fn>
    void trace (message_flow_outcome_t outcome,
                message_flow_result_t result,
                Fn &&build_event) const noexcept
    {
        const auto effective_mode =
          dispatch_options_access_t::effective_message_flow (*_options);
        const auto required = result == message_flow_result_t::succeeded
                                ? required_mode (outcome)
                                : message_flow_log_mode_t::errors;
        if (rank (effective_mode) < rank (required) || !has_sink ()
            || (result == message_flow_result_t::succeeded
                && outcome != message_flow_outcome_t::dropped
                && outcome != message_flow_outcome_t::backpressured
                && !sample_current (std::nullopt))) {
            return;
        }
        try {
            auto event = build_event ();
            event.result = result;
            stamp_flow (event);
            emit (std::move (event), effective_mode);
        }
        catch (...) {
            observer_failure_count ().fetch_add (1, std::memory_order_relaxed);
        }
    }

    template <typename Fn>
    void trace (message_flow_outcome_t outcome,
                const std::optional<message_flow_result_t> &result,
                Fn &&build_event) const noexcept
    {
        if (result) {
            trace (outcome, *result, std::forward<Fn> (build_event));
            return;
        }
        trace (outcome, std::forward<Fn> (build_event));
    }

    template <typename Fn>
    void trace (message_flow_log_mode_t min_mode,
                message_flow_outcome_t outcome,
                Fn &&build_event) const noexcept
    {
        const auto effective_mode =
          dispatch_options_access_t::effective_message_flow (*_options);
        const auto required = rank (min_mode) > rank (required_mode (outcome))
                                ? min_mode
                                : required_mode (outcome);
        if (rank (effective_mode) < rank (required) || !has_sink ()
            || (outcome != message_flow_outcome_t::dropped
                && outcome != message_flow_outcome_t::backpressured
                && !sample_current (std::nullopt))) {
            return;
        }
        // build_event() builds the event (allocates strings); guard it so a throw
        // (e.g. bad_alloc) never terminates this noexcept tracing call.
        try {
            auto event = build_event ();
            stamp_flow (event);
            emit (std::move (event), effective_mode);
        }
        catch (...) {
            observer_failure_count ().fetch_add (1, std::memory_order_relaxed);
        }
    }

    template <typename Fn>
    void trace_with_sampling_key (message_flow_log_mode_t min_mode,
                                  message_flow_outcome_t outcome,
                                  std::string_view sampling_flow_id,
                                  Fn &&build_event) const noexcept
    {
        const auto effective_mode =
          dispatch_options_access_t::effective_message_flow (*_options);
        const auto required = rank (min_mode) > rank (required_mode (outcome))
                                ? min_mode
                                : required_mode (outcome);
        if (rank (effective_mode) < rank (required) || !has_sink ()
            || (outcome != message_flow_outcome_t::dropped
                && outcome != message_flow_outcome_t::backpressured
                && !sample_current (sampling_flow_id)))
            return;
        try {
            auto event = build_event ();
            stamp_flow (event);
            emit (std::move (event), effective_mode);
        }
        catch (...) {
            observer_failure_count ().fetch_add (1, std::memory_order_relaxed);
        }
    }

    // Eager form: prefer the lazy overload on hot paths. This still gates before
    // doing any work, but the caller has already paid to build the event.
    void trace (message_flow_event_t event) const noexcept
    {
        const auto effective_mode =
          dispatch_options_access_t::effective_message_flow (*_options);
        const auto required = event.result
                                  && *event.result != message_flow_result_t::succeeded
                                ? message_flow_log_mode_t::errors
                                : required_mode (event.outcome);
        if (rank (effective_mode) < rank (required) || !has_sink ()) {
            return;
        }
        stamp_flow (event);
        if (event.outcome != message_flow_outcome_t::dropped
            && event.outcome != message_flow_outcome_t::backpressured
            && (!event.result
                || *event.result == message_flow_result_t::succeeded)
            && !sample (_options->diagnostics.sample_rate (), event)) {
            return;
        }
        emit (std::move (event), effective_mode);
    }

  private:
    bool has_sink () const noexcept
    {
        return dispatch_options_access_t::logger (*_options).has_value ()
               || dispatch_options_access_t::has_observer (*_options);
    }

    void emit (message_flow_event_t event,
               message_flow_log_mode_t effective_mode) const noexcept
    {
        traced_count ().fetch_add (1, std::memory_order_relaxed);
        if (dispatch_options_access_t::logger (*_options))
            log_default (event, effective_mode);
        try {
            dispatch_options_access_t::observe (*_options, event);
        }
        catch (...) {
            observer_failure_count ().fetch_add (1, std::memory_order_relaxed);
        }
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

    static const char *terminal_outcome_name (
      message_flow_outcome_t outcome) noexcept
    {
        switch (outcome) {
            case message_flow_outcome_t::dropped:
                return "dropped";
            case message_flow_outcome_t::backpressured:
                return "backpressured";
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
                if (!flow->flow_id.empty ()) {
                    event.flow_id = flow->flow_id;
                    event.flow_origin = flow->origin;
                }
            }
        }
    }

    /* Flow-consistent sampling. Without a flow id the key is the source MeshNode
     * generation plus that source's shared local sequence, never a process-global
     * stride counter. */
    bool sample (double rate, const message_flow_event_t &event) const noexcept
    {
        if (rate >= 1.0) {
            return true;
        }
        if (rate <= 0.0) {
            return false;
        }
        std::uint32_t hash = 0x811c9dc5u;
        auto add = [&hash] (std::uint8_t byte) {
            hash ^= static_cast<std::uint8_t> (byte);
            hash *= 0x01000193u;
        };
        if (event.flow_id) {
            for (const auto byte : *event.flow_id)
                add (static_cast<std::uint8_t> (byte));
        } else {
            const std::array<std::uint64_t, 2> key{
              event.source_mesh_generation.value_or (
                dispatch_options_access_t::source_mesh_generation (*_options)),
              dispatch_options_access_t::next_local_sampling_sequence (*_options)};
            for (const auto value : key) {
                for (unsigned int shift = 0; shift != 64; shift += 8)
                    add (static_cast<std::uint8_t> (value >> shift));
            }
        }
        return static_cast<double> (hash) / 4294967296.0 < rate;
    }

    bool sample_current (
      std::optional<std::string_view> explicit_flow_id) const noexcept
    {
        const double rate = _options->diagnostics.sample_rate ();
        if (rate >= 1.0)
            return true;
        if (rate <= 0.0)
            return false;

        std::uint32_t hash = 0x811c9dc5u;
        auto add = [&hash] (std::uint8_t byte) {
            hash ^= byte;
            hash *= 0x01000193u;
        };
        if (explicit_flow_id && !explicit_flow_id->empty ()) {
            for (const auto byte : *explicit_flow_id)
                add (static_cast<std::uint8_t> (byte));
        } else if (const auto &flow = runtime::flow_context_t::current ();
                   flow && !flow->flow_id.empty ()) {
            for (const auto byte : flow->flow_id)
                add (static_cast<std::uint8_t> (byte));
        } else {
            const std::array<std::uint64_t, 2> key{
              dispatch_options_access_t::source_mesh_generation (*_options),
              dispatch_options_access_t::next_local_sampling_sequence (*_options)};
            for (const auto value : key) {
                for (unsigned int shift = 0; shift != 64; shift += 8)
                    add (static_cast<std::uint8_t> (value >> shift));
            }
        }
        return static_cast<double> (hash) / 4294967296.0 < rate;
    }

    void log_default (const message_flow_event_t &event,
                      message_flow_log_mode_t effective_mode) const noexcept
    {
        try {
            // Build structured key/value fields once (so collectors can ingest
            // without parsing text); reuse them for the clog fallback too.
            std::vector<log_field_t> fields;
            fields.reserve (16);
            auto add = [&fields] (const char *key, std::string value) {
                diagnostic_event_sink_t::append_field (fields, key, std::move (value));
            };
            add ("event_id", "zlink.message_flow");
            add ("phase", std::string (enum_name (event.outcome)));
            add ("outcome", event.result
                              ? std::string (enum_name (*event.result))
                              : terminal_outcome_name (event.outcome));
            add ("surface", std::string (enum_name (event.surface)));
            add ("kind", std::string (enum_name (event.message_kind)));
            if (event.packet_name) {
                add ("packet", *event.packet_name);
            }
            if (event.channel_name) {
                add ("channel", *event.channel_name);
            }
            if (event.channel_route_kind) {
                add ("channel_route", *event.channel_route_kind);
            } else if (event.surface == dispatch_error_surface_t::route_mesh_channel) {
                add ("channel_route", "route_mesh");
            } else if (event.surface == dispatch_error_surface_t::channel) {
                add ("channel_route", "client_server");
            }
            if (event.mesh_name) {
                add ("mesh", *event.mesh_name);
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
                add ("origin", std::string (enum_name (*event.flow_origin)));
            }
            if (event.source_rid) {
                add ("source_rid", *event.source_rid);
            }
            if (event.target_rid) {
                add ("target_rid", *event.target_rid);
            }
            if (event.server_rid) {
                add ("server_rid", *event.server_rid);
            }
            if (event.spot_id) {
                add ("spot", *event.spot_id);
            }
            if (event.actor_id) {
                add ("actor", *event.actor_id);
            }
            if (event.instance_spot_type) {
                add ("instance_type", *event.instance_spot_type);
            }
            if (event.activation_state) {
                add ("activation_state", *event.activation_state);
            }
            if (event.reason) {
                add ("reason", std::string (enum_name (*event.reason)));
            } else if (event.error_reason) {
                add ("reason", std::string (enum_name (*event.error_reason)));
            }
            if (event.message_size && effective_mode == message_flow_log_mode_t::detailed
                && _options->diagnostics.include_message_sizes ()) {
                add ("size", std::to_string (*event.message_size));
            }
            if (effective_mode == message_flow_log_mode_t::detailed) {
                if (event.detail_stage)
                    add ("stage", *event.detail_stage);
                if (event.detail_result)
                    add ("result", *event.detail_result);
            }
            // Emit structured fields only through an explicitly configured
            // framework logger; observer-only and no-sink paths stay silent.
            diagnostic_event_sink_t::log_if_configured (
              dispatch_options_access_t::logger (*_options), log_level_t::info,
              "message flow", std::move (fields));
        }
        catch (...) {
            observer_failure_count ().fetch_add (1, std::memory_order_relaxed);
        }
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
