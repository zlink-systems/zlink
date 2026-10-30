/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <zlink/framework/contracts/dispatch/execution.hpp>

#include "runtime/diagnostics/diagnostic_event_sink.hpp"
#include "runtime/diagnostics/dispatch_diagnostics_names.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/diagnostics/flow_context.hpp"

#include <array>
#include <limits>
#include <atomic>
#include <cstdint>
#include <exception>
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
// sharing the process telemetry delivery owner so errors and healthy transitions
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
    explicit message_flow_tracer_t (const dispatch_options_t &options) : _options (&options) {}

    bool enabled (message_flow_log_mode_t min_mode) const noexcept
    {
        const auto mode = dispatch_options_access_t::effective_message_flow (*_options);
        return rank (mode) >= rank (min_mode)
               && process_telemetry_has_provider (*_options, telemetry_event_kind_t::message_flow);
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
        return rank (mode ()) >= rank (message_flow_log_mode_t::errors)
               && process_telemetry_has_provider (*_options, telemetry_event_kind_t::any);
    }
    // Flow fields are created, read and propagated only while flow tracing is
    // on; flow_context_t::tracing owns that decision.
    bool flow_tracing () const noexcept { return runtime::flow_context_t::tracing (mode ()); }

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
        trace_with_sampling_key (required_mode (outcome), outcome, sampling_flow_id,
                                 std::forward<Fn> (build_event));
    }

    template <typename Fn>
    void trace (message_flow_outcome_t outcome,
                message_flow_result_t result,
                Fn &&build_event) const noexcept
    {
        const auto effective_mode = dispatch_options_access_t::effective_message_flow (*_options);
        const auto required = result == message_flow_result_t::succeeded
                                ? required_mode (outcome)
                                : message_flow_log_mode_t::errors;
        if (rank (effective_mode) < rank (required)
            || !process_telemetry_has_provider (*_options, telemetry_event_kind_t::message_flow)
            || (result == message_flow_result_t::succeeded
                && outcome != message_flow_outcome_t::dropped
                && outcome != message_flow_outcome_t::backpressured
                && !sample_current (std::nullopt))) {
            return;
        }
        try {
            auto event = build_event ();
            event.result = result;
            stamp_flow (event, effective_mode);
            emit (std::move (event), effective_mode);
        }
        catch (...) {
            record_observer_failure ();
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
        const auto effective_mode = dispatch_options_access_t::effective_message_flow (*_options);
        const auto required =
          rank (min_mode) > rank (required_mode (outcome)) ? min_mode : required_mode (outcome);
        if (rank (effective_mode) < rank (required)
            || !process_telemetry_has_provider (*_options, telemetry_event_kind_t::message_flow)
            || (outcome != message_flow_outcome_t::dropped
                && outcome != message_flow_outcome_t::backpressured
                && !sample_current (std::nullopt))) {
            return;
        }
        // build_event() builds the event (allocates strings); guard it so a throw
        // (e.g. bad_alloc) never terminates this noexcept tracing call.
        try {
            auto event = build_event ();
            stamp_flow (event, effective_mode);
            emit (std::move (event), effective_mode);
        }
        catch (...) {
            record_observer_failure ();
        }
    }

    template <typename Fn>
    void trace_with_sampling_key (message_flow_log_mode_t min_mode,
                                  message_flow_outcome_t outcome,
                                  std::string_view sampling_flow_id,
                                  Fn &&build_event) const noexcept
    {
        const auto effective_mode = dispatch_options_access_t::effective_message_flow (*_options);
        const auto required =
          rank (min_mode) > rank (required_mode (outcome)) ? min_mode : required_mode (outcome);
        if (rank (effective_mode) < rank (required)
            || !process_telemetry_has_provider (*_options, telemetry_event_kind_t::message_flow)
            || (outcome != message_flow_outcome_t::dropped
                && outcome != message_flow_outcome_t::backpressured
                && !sample_current (sampling_flow_id)))
            return;
        try {
            auto event = build_event ();
            stamp_flow (event, effective_mode);
            emit (std::move (event), effective_mode);
        }
        catch (...) {
            record_observer_failure ();
        }
    }

    // Eager form: prefer the lazy overload on hot paths. This still gates before
    // doing any work, but the caller has already paid to build the event.
    void trace (message_flow_event_t event) const noexcept
    {
        const auto effective_mode = dispatch_options_access_t::effective_message_flow (*_options);
        const auto required = event.result && *event.result != message_flow_result_t::succeeded
                                ? message_flow_log_mode_t::errors
                                : required_mode (event.outcome);
        if (rank (effective_mode) < rank (required)
            || !process_telemetry_has_provider (*_options, telemetry_event_kind_t::message_flow)) {
            return;
        }
        stamp_flow (event, effective_mode);
        if (event.outcome != message_flow_outcome_t::dropped
            && event.outcome != message_flow_outcome_t::backpressured
            && (!event.result || *event.result == message_flow_result_t::succeeded)
            && !sample (_options->diagnostics.sample_rate (), event)) {
            return;
        }
        emit (std::move (event), effective_mode);
    }

  private:
    void emit (message_flow_event_t event, message_flow_log_mode_t effective_mode) const noexcept
    {
        traced_count ().fetch_add (1, std::memory_order_relaxed);
        dispatch_options_access_t::submit (*_options, std::move (event), effective_mode);
    }

  public:
    static std::uint64_t traced () noexcept
    {
        return traced_count ().load (std::memory_order_relaxed);
    }

    static void record_observer_failure () noexcept
    {
        observer_failure_count ().fetch_add (1, std::memory_order_relaxed);
    }

    static std::uint64_t observer_failures () noexcept
    {
        return observer_failure_count ().load (std::memory_order_relaxed);
    }

    static std::uint64_t observer_dropped () noexcept { return telemetry_dropped (); }

  private:
    static int rank (message_flow_log_mode_t mode) noexcept { return static_cast<int> (mode); }

    /* Fills the optional flow pair from the ambient context when the caller
     * did not carry it (flow-correlation §8: both or neither). */
    static void stamp_flow (message_flow_event_t &event, message_flow_log_mode_t effective_mode)
    {
        if (!runtime::flow_context_t::tracing (effective_mode)) {
            event.flow_id.reset ();
            event.flow_origin.reset ();
            return;
        }
        if (!event.stream_session_id) {
            if (const auto &flow = runtime::flow_context_t::current ();
                flow && flow->stream_session_id)
                event.stream_session_id = *flow->stream_session_id;
        }
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
    static constexpr std::uint32_t sampling_hash_offset_basis = 0x811c9dc5u;
    static constexpr std::uint32_t sampling_hash_prime = 0x01000193u;
    static void append_sampling_hash (std::uint32_t &hash, std::uint8_t byte) noexcept
    {
        hash ^= byte;
        hash *= sampling_hash_prime;
    }

    bool sample (double rate, const message_flow_event_t &event) const noexcept
    {
        if (rate >= 1.0) {
            return true;
        }
        if (rate <= 0.0) {
            return false;
        }
        std::uint32_t hash = sampling_hash_offset_basis;
        auto add = [&hash] (std::uint8_t byte) { append_sampling_hash (hash, byte); };
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
        return static_cast<double> (hash)
                 / (static_cast<double> (std::numeric_limits<std::uint32_t>::max ()) + 1.0)
               < rate;
    }

    bool sample_current (std::optional<std::string_view> explicit_flow_id) const noexcept
    {
        const double rate = _options->diagnostics.sample_rate ();
        if (rate >= 1.0)
            return true;
        if (rate <= 0.0)
            return false;

        std::uint32_t hash = sampling_hash_offset_basis;
        auto add = [&hash] (std::uint8_t byte) { append_sampling_hash (hash, byte); };
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
        return static_cast<double> (hash)
                 / (static_cast<double> (std::numeric_limits<std::uint32_t>::max ()) + 1.0)
               < rate;
    }

    static std::atomic<std::uint64_t> &traced_count () noexcept
    {
        return telemetry_traced_count ();
    }

    static std::atomic<std::uint64_t> &observer_failure_count () noexcept
    {
        return telemetry_failure_count ();
    }

    const dispatch_options_t *_options;
};

} // namespace zlink::framework::detail
